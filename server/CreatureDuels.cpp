/*
 * CreatureDuels.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "CreatureDuels.h"

#include "BattlegroundGame.h"
#include "CGameHandler.h"
#include "IGameServer.h"
#include "battles/BattleProcessor.h"

#include "../lib/CCreatureHandler.h"
#include "../lib/CStack.h"
#include "../lib/GameLibrary.h"
#include "../lib/battle/AutocombatPreferences.h"
#include "../lib/battle/BattleAction.h"
#include "../lib/battle/BattleInfo.h"
#include "../lib/battle/BattleLayout.h"
#include "../lib/bonuses/BonusEnum.h"
#include "../lib/callback/AIFactory.h"
#include "../lib/callback/CBattleCallback.h"
#include "../lib/callback/CBattleGameInterface.h"
#include "../lib/callback/GameRandomizer.h"
#include "../lib/callback/IClient.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapObjects/army/CCommanderInstance.h"
#include "../lib/mapObjects/army/CStackInstance.h"
#include "../lib/mapping/CMap.h"
#include "../lib/modding/ModScope.h"
#include "../lib/networkPacks/PacksForClientBattle.h"
#include "../lib/networkPacks/PacksForServer.h"

#include <vcmi/Environment.h>

namespace
{

/// Total value of units on each side at the start of a battle, in units of configured AI value
constexpr double armyBudget = 60000;
/// Battles at each searched number of opponents
constexpr int battlesPerStep = 3;
constexpr int maxSteps = 6;
/// Step in number of opponents, as a factor, under which the search counts as converged
constexpr double convergedStep = 1.07;
/// Most that one step may change the number of opponents by, as a factor either way
constexpr double maxStepFactor = 4.0;
/// Furthest that the searched number of opponents may get from the one configured values suggest
constexpr double maxPriorFactor = 30.0;
/// Fewest units the measured creature is fielded with, so that one unit dying is not a large step
constexpr int minSubjectUnits = 5;
/// Most units either side is fielded with
constexpr int maxUnits = 100000;
/// Share of health left above which a side counts as untouched by the battle
constexpr double untouchedShare = 0.95;

constexpr int maxRounds = 40;
constexpr int maxActions = 1000;
constexpr auto maxBattleDuration = std::chrono::seconds(180);

/// Health of the units a side started a battle with, ignoring summoned units and clones
struct SideHealth
{
	int64_t initial = 0;
	int64_t left = 0;

	double shareLeft() const
	{
		return initial > 0 ? static_cast<double>(left) / initial : 0.0;
	}
};

struct BattleOutcome
{
	BattleSideArray<SideHealth> health;
	/// Battle reached a limit on rounds, actions or time before either side won, or a side fled
	bool limited = false;
	/// Actions of the battle AI that the server rejected and replaced with defending
	int rejected = 0;
	int rounds = 0;
};

/// Event the battle loop has to process: a unit waiting for its player, or an action sent by a player
struct PendingEvent
{
	bool isAction = false;
	BattleID battleID;
	PlayerColor player;
	/// Unit waiting for its player
	uint32_t unit = 0;
	BattleAction action;
};

uint32_t stableHash(const std::string & text)
{
	// FNV-1a, so that the same pair picks the same references and random seeds on every platform
	uint32_t hash = 2166136261u;
	for(char symbol : text)
	{
		hash ^= static_cast<uint8_t>(symbol);
		hash *= 16777619u;
	}
	return hash;
}

int levelOf(const CCreature * creature)
{
	return std::clamp(creature->getLevel(), 1, 7);
}

/// Configured value of one unit, used to size armies and as the starting point of the search
double configuredValue(const CCreature * creature)
{
	if(creature->getAIValue() > 0)
		return creature->getAIValue();
	if(creature->getFightValue() > 0)
		return creature->getFightValue();
	return 100.0 * levelOf(creature);
}

bool isMeasurable(const CCreature * creature)
{
	// war machines neither move nor fight on their own, and special creatures are not real units
	return !creature->special && !creature->hasBonusOfType(BonusType::SIEGE_WEAPON);
}

/// Server for one game in which battles are played, with the battle AI of both players in the
/// same process. Packs a client would receive go straight to the AI, and its requests are queued
/// and processed one by one, so that a battle never recurses through its own actions
class DuelServer final : public IGameServer, public IClient
{
public:
	DuelServer(const std::string & battleAI, int experienceRank);
	~DuelServer();

	BattleOutcome play(const CCreature * left, int leftCount, const CCreature * right, int rightCount, int seed);

	void setState(EServerState value) override { state = value; }
	EServerState getState() const override { return state; }
	bool isPlayerHost(const PlayerColor & color) const override { return false; }
	bool hasPlayerAt(PlayerColor player, GameConnectionID connectionID) const override { return true; }
	bool hasBothPlayersAtSameConnection(PlayerColor left, PlayerColor right) const override { return true; }
	void applyPack(CPackForClient & pack) override;
	void sendPack(CPackForClient & pack, GameConnectionID connectionID) override {}

	std::optional<BattleAction> makeSurrenderRetreatDecision(PlayerColor player, const BattleID & battleID, const BattleStateInfoForRetreat & battleState) override { return std::nullopt; }
	int sendRequest(const CPackForServer & request, PlayerColor player, bool waitTillRealize) override;

private:
	EServerState state = EServerState::GAMEPLAY;
	int experienceRank;
	std::string battleAI;

	std::unique_ptr<BattlegroundGame> game;
	std::unique_ptr<CGameHandler> handler;
	std::shared_ptr<Environment> environment;
	BattleSideArray<std::shared_ptr<CBattleGameInterface>> interfaces;
	BattleSideArray<std::shared_ptr<CBattleCallback>> callbacks;

	std::deque<PendingEvent> pending;
	std::optional<BattleAction> currentAction;
	BattleID currentBattle = BattleID::NONE;
	bool battleEnded = false;
	EBattleResult battleResult = EBattleResult::NORMAL;
	int requestCounter = 0;

	static BattleSide sideOf(const PlayerColor & player);

	void createInterfaces();
	void removeBattleParticipants();
	void setArmy(BattleSide side, const CCreature * creature, int count);
	void replaceBattleSetup(BattleStart & pack) const;
	void onBattleStarted(const BattleID & battleID);
	SideHealth healthOf(const BattleInfo & battle, BattleSide side) const;
};

DuelServer::DuelServer(const std::string & battleAI, int experienceRank)
	: experienceRank(experienceRank)
	, battleAI(battleAI)
	, game(std::make_unique<BattlegroundGame>())
	, handler(std::make_unique<CGameHandler>(*this, game->gameState))
	// the game handler outlives every interface, so the environment does not own it
	, environment(handler.get(), [](Environment *){})
{
	removeBattleParticipants();

	handler->battles->resultInterceptor = [this](const BattleID &, EBattleResult result, BattleSide)
	{
		battleEnded = true;
		battleResult = result;
	};
}

void DuelServer::createInterfaces()
{
	// a battle AI keeps some state between battles, e.g. how long it has been only defending,
	// so every battle gets a fresh one and no battle depends on the ones before it
	interfaces = {};
	callbacks = {};

	AutocombatPreferences preferences;
	// heroes are stripped of everything, and casting would be a contribution of the hero
	preferences.enableSpellsUsage = false;
	preferences.enableTacticsUsage = false;

	for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		const PlayerColor player = game->heroes[side]->getOwner();
		interfaces[side] = AIFactory::createBattleAI(battleAI);
		callbacks[side] = std::make_shared<CBattleCallback>(player, this);
		interfaces[side]->playerID = player;
		interfaces[side]->initBattleInterface(environment, callbacks[side], preferences);
	}
}

DuelServer::~DuelServer()
{
	// interfaces keep callbacks of a battle that may still be in the game state
	interfaces = {};
	callbacks = {};
	handler.reset();
	game.reset();
}

BattleSide DuelServer::sideOf(const PlayerColor & player)
{
	return player == PlayerColor(0) ? BattleSide::ATTACKER : BattleSide::DEFENDER;
}

void DuelServer::removeBattleParticipants()
{
	for(auto * hero : game->heroes)
	{
		// war machines and spellbook come as artifacts, and would join the battle or let the hero cast
		std::vector<ArtifactPosition> worn;
		for(const auto & [position, slot] : hero->artifactsWorn)
			if(!slot.locked)
				worn.push_back(position);

		for(const auto & position : worn)
			if(hero->getArt(position))
				game->gameState->getMap().removeArtifactInstance(*hero, position);

		while(!hero->artifactsInBackpack.empty())
			game->gameState->getMap().removeArtifactInstance(*hero, ArtifactPosition::BACKPACK_START);

		if(auto * commander = hero->getCommander())
			commander->setAlive(false);

		hero->mana = 0;
	}
}

void DuelServer::setArmy(BattleSide side, const CCreature * creature, int count)
{
	CGHeroInstance * hero = game->heroes[side];
	hero->clearSlots();

	auto stack = std::make_unique<CStackInstance>(game->gameState.get(), creature->getId(), count);

	if(experienceRank > 0)
	{
		const auto & ranks = LIBRARY->creh->expRanks.at(vstd::iswithin(creature->getLevel(), 1, 7) ? creature->getLevel() : 0);
		const int rank = std::clamp<int>(experienceRank, 1, static_cast<int>(ranks.size()) - 1);
		stack->giveTotalStackExperience(static_cast<TExpType>(ranks.at(rank - 1)) * count);
	}

	hero->putStack(SlotID(0), std::move(stack));
}

BattleOutcome DuelServer::play(const CCreature * left, int leftCount, const CCreature * right, int rightCount, int seed)
{
	setArmy(BattleSide::ATTACKER, left, leftCount);
	setArmy(BattleSide::DEFENDER, right, rightCount);

	pending.clear();
	currentAction.reset();
	currentBattle = BattleID::NONE;
	battleEnded = false;
	battleResult = EBattleResult::NORMAL;
	handler->randomizer->setSeed(seed);
	createInterfaces();

	const CGHeroInstance * attacker = game->heroes[BattleSide::ATTACKER];
	const CGHeroInstance * defender = game->heroes[BattleSide::DEFENDER];
	const BattleLayout layout = BattleLayout::createDefaultLayout(handler->gameInfo(), attacker, defender);

	handler->battles->startBattle(attacker, defender, defender->visitablePos(), attacker, defender, layout, nullptr);

	const BattleInfo * battle = game->gameState->getBattle(currentBattle);
	if(!battle)
		throw std::runtime_error("Battle failed to start");

	BattleOutcome outcome;
	const auto startTime = std::chrono::steady_clock::now();
	int actions = 0;

	while(!battleEnded)
	{
		if(battle->getRound() > maxRounds || actions > maxActions || std::chrono::steady_clock::now() - startTime > maxBattleDuration)
		{
			outcome.limited = true;
			break;
		}

		if(pending.empty())
			throw std::runtime_error("Battle stalled with no unit to act");

		const PendingEvent event = pending.front();
		pending.pop_front();

		if(event.isAction)
		{
			++actions;
			const bool accepted = handler->battles->makePlayerBattleAction(event.battleID, event.player, event.action);

			// a rejected action leaves the unit waiting for an action that never comes
			if(!accepted && !battleEnded && pending.empty())
			{
				const CStack * active = battle->battleGetStackByID(battle->activeStack, false);
				if(!active)
					throw std::runtime_error("Battle AI action rejected with no unit to act");

				++outcome.rejected;
				if(!handler->battles->makePlayerBattleAction(event.battleID, event.player, BattleAction::makeDefend(active)))
					throw std::runtime_error("Battle AI action rejected, and defending as well");
			}
		}
		else
		{
			// stale if another unit was activated in the meantime
			if(event.unit != battle->activeStack)
				continue;

			const CStack * active = battle->battleGetStackByID(event.unit, false);
			if(!active)
				throw std::runtime_error("Active unit of the battle not found");

			interfaces[sideOf(event.player)]->activeStack(event.battleID, active);
		}
	}

	if(battleResult != EBattleResult::NORMAL)
		outcome.limited = true;

	outcome.rounds = battle->getRound();
	for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		outcome.health[side] = healthOf(*battle, side);

	handler->battles->discardBattle(currentBattle);
	return outcome;
}

SideHealth DuelServer::healthOf(const BattleInfo & battle, BattleSide side) const
{
	SideHealth result;

	for(const auto & stack : battle.stacks)
	{
		if(stack->unitSide() != side || stack->summoned || stack->isClone())
			continue;

		result.initial += static_cast<int64_t>(stack->unitBaseAmount()) * stack->getMaxHealth();
		if(stack->alive())
			result.left += stack->getAvailableHealth();
	}

	return result;
}

void DuelServer::replaceBattleSetup(BattleStart & pack) const
{
	const BattleInfo & proposed = *pack.info;

	BattleSideArray<const CArmedInstance *> armies = {proposed.getSideArmy(BattleSide::ATTACKER), proposed.getSideArmy(BattleSide::DEFENDER)};
	BattleSideArray<const CGHeroInstance *> heroes = {proposed.getSideHero(BattleSide::ATTACKER), proposed.getSideHero(BattleSide::DEFENDER)};
	const auto terrain = game->gameState->getTile(proposed.tile)->getTerrainID();

	// the battlefield of the terrain may come from a mod and carry bonuses of its own
	auto replacement = BattleInfo::setupBattle(game->gameState.get(), proposed.tile, terrain, BattlegroundGame::neutralBattlefield(), armies, heroes, proposed.getLayout(), nullptr);
	replacement->replayAllowed = proposed.replayAllowed;
	// obstacles are placed at random, which only adds noise between battles of a pair
	replacement->obstacles.clear();
	replacement->tacticDistance = 0;

	pack.info = std::move(replacement);
}

void DuelServer::applyPack(CPackForClient & pack)
{
	if(auto * start = dynamic_cast<BattleStart *>(&pack))
		replaceBattleSetup(*start);

	game->gameState->apply(pack);

	if(auto * start = dynamic_cast<BattleStart *>(&pack))
	{
		onBattleStarted(start->battleID);
	}
	else if(auto * activation = dynamic_cast<BattleSetActiveStack *>(&pack))
	{
		if(activation->reason == BattleUnitTurnReason::AUTOMATIC_ACTION)
			return;

		const BattleInfo * battle = game->gameState->getBattle(activation->battleID);
		const CStack * stack = battle->battleGetStackByID(activation->stack);

		PendingEvent event;
		event.battleID = activation->battleID;
		event.unit = activation->stack;
		// player of the other side, if the unit is hypnotized
		event.player = battle->battleGetOwner(stack);
		pending.push_back(event);
	}
	else if(auto * started = dynamic_cast<StartAction *>(&pack))
	{
		currentAction = started->ba;
	}
	else if(auto * ended = dynamic_cast<EndAction *>(&pack))
	{
		if(currentAction)
			for(const auto & battleInterface : interfaces)
				battleInterface->actionFinished(ended->battleID, *currentAction);
		currentAction.reset();
	}
	else if(auto * cancelled = dynamic_cast<BattleCancelled *>(&pack))
	{
		for(const auto & callback : callbacks)
			if(callback->getActiveBattles().count(cancelled->battleID))
				callback->onBattleEnded(cancelled->battleID);
	}
}

void DuelServer::onBattleStarted(const BattleID & battleID)
{
	currentBattle = battleID;
	const BattleInfo * info = game->gameState->getBattle(battleID);

	for(const auto & callback : callbacks)
		callback->onBattleStarted(info);

	for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
	{
		interfaces[side]->battleStart(battleID,
			info->getSideArmy(BattleSide::ATTACKER), info->getSideArmy(BattleSide::DEFENDER), info->tile,
			info->getSideHero(BattleSide::ATTACKER), info->getSideHero(BattleSide::DEFENDER), side, false);
	}
}

int DuelServer::sendRequest(const CPackForServer & request, PlayerColor player, bool waitTillRealize)
{
	const auto * makeAction = dynamic_cast<const MakeAction *>(&request);
	if(!makeAction)
	{
		logGlobal->warn("Creature duels: battle AI sent a request that is not a battle action");
		return ++requestCounter;
	}

	PendingEvent event;
	event.isAction = true;
	event.battleID = makeAction->battleID;
	event.player = player;
	event.action = makeAction->ba;
	pending.push_back(event);
	return ++requestCounter;
}

/// One measured creature against one creature of the original game
struct Job
{
	const CCreature * subject = nullptr;
	const CCreature * reference = nullptr;

	std::string key(int rank) const
	{
		return subject->getJsonKey() + "," + reference->getJsonKey() + "," + std::to_string(rank);
	}
};

struct JobResult
{
	int subjectUnits = 0;
	double startingUnits = 0;
	double equilibriumUnits = 0;
	double spread = 0;
	int battles = 0;
	int valid = 0;
	int limited = 0;
	int rejected = 0;
	int steps = 0;
	std::string status = "ok";
};

class DuelRunner
{
public:
	explicit DuelRunner(const CreatureDuels::Options & options);
	void run();

private:
	const CreatureDuels::Options & options;
	std::unique_ptr<DuelServer> server;

	std::vector<Job> jobs;
	std::set<std::string> done;

	boost::filesystem::path markerPath() const;
	void collectJobs();
	void loadDone();
	void recoverCrashedJob();
	JobResult measure(const Job & job);
	void writeResult(const Job & job, const JobResult & result, double seconds);
};

DuelRunner::DuelRunner(const CreatureDuels::Options & options)
	: options(options)
{
}

boost::filesystem::path DuelRunner::markerPath() const
{
	return boost::filesystem::path(options.output.string() + ".current");
}

void DuelRunner::collectJobs()
{
	std::set<std::string> wanted;
	if(!options.subjects.empty())
	{
		std::ifstream file(options.subjects.c_str());
		std::string line;
		while(std::getline(file, line))
		{
			boost::algorithm::trim(line);
			if(!line.empty())
				wanted.insert(line);
		}
	}

	std::array<std::vector<const CCreature *>, 8> referencesByLevel;
	std::vector<const CCreature *> subjects;

	for(const auto & creature : LIBRARY->creh->objects)
	{
		if(!creature || !isMeasurable(creature.get()))
			continue;

		if(creature->getModScope() == ModScope::scopeBuiltin() && vstd::iswithin(creature->getLevel(), 1, 7))
			referencesByLevel.at(creature->getLevel()).push_back(creature.get());

		if(wanted.empty() || wanted.count(creature->getJsonKey()))
			subjects.push_back(creature.get());
	}

	int subjectIndex = 0;
	for(const auto * subject : subjects)
	{
		if(subjectIndex++ % options.shardCount != options.shardIndex)
			continue;

		std::vector<const CCreature *> candidates;
		for(const auto * reference : referencesByLevel.at(levelOf(subject)))
			if(reference != subject)
				candidates.push_back(reference);

		if(candidates.empty())
			continue;

		// spread evenly over the level, starting at a place that differs between creatures
		const size_t count = std::min<size_t>(options.references, candidates.size());
		const size_t offset = stableHash(subject->getJsonKey()) % candidates.size();
		for(size_t index = 0; index < count; ++index)
			jobs.push_back({subject, candidates.at((offset + index * candidates.size() / count) % candidates.size())});
	}
}

void DuelRunner::loadDone()
{
	std::ifstream file(options.output.c_str());
	std::string line;
	while(std::getline(file, line))
	{
		std::vector<std::string> fields;
		boost::split(fields, line, boost::is_any_of(","));
		if(fields.size() > 3)
			done.insert(fields[0] + "," + fields[1] + "," + fields[2]);
	}
}

void DuelRunner::recoverCrashedJob()
{
	// a pair that brought the whole process down would do it again, so it is recorded and skipped
	std::ifstream marker(markerPath().c_str());
	std::string key;
	if(!std::getline(marker, key) || key.empty() || done.count(key))
		return;

	for(const auto & job : jobs)
	{
		if(job.key(options.experienceRank) != key)
			continue;

		JobResult result;
		result.status = "crashed";
		writeResult(job, result, 0);
		done.insert(key);
		logGlobal->warn("Creature duels: %s crashed the previous run and is skipped", key);
	}
}

JobResult DuelRunner::measure(const Job & job)
{
	JobResult result;

	const double subjectValue = configuredValue(job.subject);
	const double referenceValue = configuredValue(job.reference);

	result.subjectUnits = std::clamp(static_cast<int>(std::lround(armyBudget / subjectValue)), minSubjectUnits, maxUnits);
	result.startingUnits = result.subjectUnits * subjectValue / referenceValue;

	const double lowest = std::max(1.0, result.startingUnits / maxPriorFactor);
	const double highest = std::min<double>(maxUnits, result.startingUnits * maxPriorFactor);
	const uint32_t seedBase = stableHash(job.key(options.experienceRank));

	double guess = std::clamp(result.startingUnits, lowest, highest);
	std::vector<double> previousStep;
	std::vector<double> lastStep;

	for(int step = 0; step < maxSteps; ++step)
	{
		const int referenceUnits = std::max(1, static_cast<int>(std::lround(guess)));
		std::vector<double> estimates;

		for(int index = 0; index < battlesPerStep; ++index)
		{
			// the measured creature alternates between sides, so that neither side's advantage stays with it
			const bool subjectAttacks = (result.battles % 2) == 0;
			const int seed = static_cast<int>(seedBase + result.battles);
			++result.battles;

			const BattleOutcome outcome = subjectAttacks
				? server->play(job.subject, result.subjectUnits, job.reference, referenceUnits, seed)
				: server->play(job.reference, referenceUnits, job.subject, result.subjectUnits, seed);

			const SideHealth & subjectHealth = outcome.health[subjectAttacks ? BattleSide::ATTACKER : BattleSide::DEFENDER];
			const SideHealth & referenceHealth = outcome.health[subjectAttacks ? BattleSide::DEFENDER : BattleSide::ATTACKER];

			if(outcome.limited)
				++result.limited;
			result.rejected += outcome.rejected;

			const double subjectLeft = std::min(subjectHealth.shareLeft(), untouchedShare);
			const double referenceLeft = std::min(referenceHealth.shareLeft(), untouchedShare);

			if(subjectHealth.shareLeft() >= untouchedShare && referenceHealth.shareLeft() >= untouchedShare)
				continue;

			// Lanchester's square law: strength of a side grows with the square of its numbers, and
			// what both sides lose of it is equal, so the health left gives the ratio of strengths
			const double strengthRatio = (1.0 - referenceLeft * referenceLeft) / (1.0 - subjectLeft * subjectLeft);
			estimates.push_back(std::log(referenceUnits) + 0.5 * std::log(strengthRatio));
			++result.valid;
		}

		result.steps = step + 1;

		if(estimates.empty())
		{
			result.status = "stalemate";
			break;
		}

		previousStep = std::move(lastStep);
		lastStep = estimates;

		const double mean = std::accumulate(estimates.begin(), estimates.end(), 0.0) / estimates.size();
		double next = std::exp(mean);
		next = std::clamp(next, referenceUnits / maxStepFactor, referenceUnits * maxStepFactor);

		if(next < lowest || next > highest)
		{
			result.status = "out-of-range";
			next = std::clamp(next, lowest, highest);
		}

		const bool converged = std::max(next / referenceUnits, referenceUnits / next) < convergedStep;
		guess = next;

		if(converged && step > 0)
			break;
	}

	std::vector<double> combined = lastStep;
	vstd::concatenate(combined, previousStep);

	if(!combined.empty())
	{
		const double mean = std::accumulate(combined.begin(), combined.end(), 0.0) / combined.size();
		double variance = 0;
		for(double value : combined)
			variance += (value - mean) * (value - mean);

		result.equilibriumUnits = std::exp(mean);
		result.spread = combined.size() > 1 ? std::sqrt(variance / (combined.size() - 1)) : 0.0;
	}

	return result;
}

void DuelRunner::writeResult(const Job & job, const JobResult & result, double seconds)
{
	const bool newFile = !boost::filesystem::exists(options.output);
	std::ofstream file(options.output.c_str(), std::ios::app);

	if(newFile)
		file << "subject,reference,rank,subjectLevel,referenceLevel,subjectAiValue,referenceAiValue,subjectFightValue,referenceFightValue,"
			"subjectUnits,startingUnits,equilibriumUnits,spread,battles,valid,limited,rejected,steps,seconds,status\n";

	file << boost::str(boost::format("%s,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.4f,%d,%d,%d,%d,%d,%.1f,%s\n")
		% job.key(options.experienceRank)
		% job.subject->getLevel()
		% job.reference->getLevel()
		% job.subject->getAIValue()
		% job.reference->getAIValue()
		% job.subject->getFightValue()
		% job.reference->getFightValue()
		% result.subjectUnits
		% result.startingUnits
		% result.equilibriumUnits
		% result.spread
		% result.battles
		% result.valid
		% result.limited
		% result.rejected
		% result.steps
		% seconds
		% result.status);
}

void DuelRunner::run()
{
	collectJobs();
	loadDone();
	recoverCrashedJob();

	const size_t remaining = std::count_if(jobs.begin(), jobs.end(), [this](const Job & job) { return !done.count(job.key(options.experienceRank)); });

	{
		std::ofstream total(boost::filesystem::path(options.output.string() + ".jobs").c_str());
		total << jobs.size() << "\n";
	}

	logGlobal->info("Creature duels: shard %d of %d, %d pairs, %d still to measure, experience rank %d",
		options.shardIndex + 1, options.shardCount, static_cast<int>(jobs.size()), static_cast<int>(remaining), options.experienceRank);

	server = std::make_unique<DuelServer>(options.battleAI, options.experienceRank);

	const auto startTime = std::chrono::steady_clock::now();
	size_t measured = 0;

	for(const auto & job : jobs)
	{
		const std::string key = job.key(options.experienceRank);
		if(done.count(key))
			continue;

		{
			std::ofstream marker(markerPath().c_str());
			marker << key << "\n";
		}

		const auto jobStart = std::chrono::steady_clock::now();
		JobResult result;

		try
		{
			result = measure(job);
		}
		catch(const std::exception & e)
		{
			result.status = std::string("error: ") + e.what();
			boost::replace_all(result.status, ",", ";");
			boost::replace_all(result.status, "\n", " ");
			logGlobal->error("Creature duels: %s failed: %s", key, e.what());

			// state of the failed battle is unknown, so the game is built again from scratch
			server.reset();
			server = std::make_unique<DuelServer>(options.battleAI, options.experienceRank);
		}

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - jobStart).count();
		writeResult(job, result, seconds);
		done.insert(key);
		++measured;

		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
		const double eta = elapsed / measured * (remaining - measured);
		logGlobal->info("Creature duels: %d/%d %s - %s, %.0f s left", static_cast<int>(measured), static_cast<int>(remaining), key, result.status, eta);
	}

	boost::filesystem::remove(markerPath());
	logGlobal->info("Creature duels complete");
}

}

void CreatureDuels::run(const Options & options)
{
	if(options.shardCount < 1 || options.shardIndex < 0 || options.shardIndex >= options.shardCount)
		throw std::runtime_error("Invalid creature duels shard");

	DuelRunner runner(options);
	runner.run();
}
