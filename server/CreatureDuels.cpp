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
#include "../lib/CSkillHandler.h"
#include "../lib/CStack.h"
#include "../lib/GameLibrary.h"
#include "../lib/battle/AutocombatPreferences.h"
#include "../lib/battle/BattleAction.h"
#include "../lib/battle/BattleInfo.h"
#include "../lib/battle/BattleLayout.h"
#include "../lib/bonuses/BonusEnum.h"
#include "../lib/bonuses/Bonus.h"
#include "../lib/CBonusTypeHandler.h"
#include "../lib/networkPacks/PacksForClientBattle.h"
#include "../lib/callback/AIFactory.h"
#include "../lib/callback/CBattleCallback.h"
#include "../lib/callback/CBattleGameInterface.h"
#include "../lib/callback/GameRandomizer.h"
#include "../lib/callback/IClient.h"
#include "../lib/entities/artifact/ArtifactUtils.h"
#include "../lib/entities/artifact/CArtHandler.h"
#include "../lib/entities/artifact/CArtifact.h"
#include "../lib/entities/artifact/CArtifactInstance.h"
#include "../lib/entities/building/CBuilding.h"
#include "../lib/entities/faction/CFaction.h"
#include "../lib/entities/faction/CTown.h"
#include "../lib/entities/faction/CTownHandler.h"
#include "../lib/entities/hero/CHero.h"
#include "../lib/entities/hero/CHeroClass.h"
#include "../lib/entities/hero/CHeroHandler.h"
#include "../lib/bonuses/Propagators.h"
#include "../lib/CRandomGenerator.h"
#include "../lib/IGameSettings.h"
#include "../lib/spells/CSpellHandler.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/json/JsonNode.h"
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
constexpr double armyBudget = 100000;
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

/// Limits after which a battle counts as limited instead of won
struct BattleLimits
{
	int rounds = 40;
	int actions = 1000;
	std::chrono::seconds duration = std::chrono::seconds(180);
};

/// One stack of an army, placed in the next free slot
struct ArmyStack
{
	const CCreature * creature = nullptr;
	int count = 0;
};

using Army = std::vector<ArmyStack>;

/// Everything a hero brings to a battle besides its army. A side without a build gets a hero
/// stripped of all of it
struct HeroBuild
{
	int level = 1;
	/// attack, defense, spell power, knowledge
	std::array<int, 4> primary = {};
	std::vector<std::pair<SecondarySkill, int>> skills;
	/// war machines, as the artifacts a hero carries them as
	std::vector<ArtifactID> warMachines;
	bool spellbook = false;
	/// Real hero whose specialty and class the side gets, NONE keeps the battleground hero's class
	HeroTypeID heroType = HeroTypeID::NONE;
	/// Spells in the spellbook; only those the hero can learn are kept, as wisdom limits them
	std::vector<SpellID> spells;
	/// Town buildings whose bonuses for all of the player's units the army gets
	std::vector<const CBuilding *> buildings;
	/// Whether the hero leads the commander of its class, raised to the hero's level
	bool commander = false;
	uint32_t commanderSeed = 0;
};

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
	int32_t unit = -1;
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
	DuelServer(const std::string & battleAI, int experienceRank, bool heroSpells = false, BattleLimits limits = {});
	~DuelServer();

	BattleOutcome play(const CCreature * left, int leftCount, const CCreature * right, int rightCount, int seed);
	BattleOutcome play(const Army & attacker, const HeroBuild * attackerHero, const Army & defender, const HeroBuild * defenderHero, int seed);
	/// Stack experience rank of each side in the next battles, instead of the server's rank; negative keeps it
	void setExperienceRanks(int attacker, int defender) { sideRanks = {attacker, defender}; }
	/// Logs what each hero brings to the next battle: specialty, commander, spells, building bonuses
	bool describeNextBattle = false;
	bool describingBattle = false;
	BattleSideArray<int> heroSpellsCast = {0, 0};
	/// Called after each pack of the running battle is applied, for battle tests that log what happens
	std::function<void(const CPackForClient &, const BattleInfo &)> observer;
	/// Called once a battle is over, before it is discarded
	std::function<void(const BattleInfo &, const BattleOutcome &)> finished;
	/// Terrain the next battles are fought on instead of the battleground's
	std::optional<TerrainId> terrain;

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
	BattleSideArray<int> sideRanks = {-1, -1};
	BattleSideArray<HeroTypeID> appliedHeroType = {HeroTypeID::NONE, HeroTypeID::NONE};
	BattleSideArray<std::vector<std::shared_ptr<Bonus>>> addedBuildingBonuses;
	std::string battleAI;
	bool heroSpells;
	BattleLimits limits;

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
	void removeArtifacts(CGHeroInstance * hero);
	void prepareHero(BattleSide side, const HeroBuild * build);
	void applyHeroType(BattleSide side, HeroTypeID type);
	void applyBuildingBonuses(BattleSide side, const HeroBuild * build);
	void applyCommander(BattleSide side, const HeroBuild * build);
	void applySpells(BattleSide side, const HeroBuild * build);
	void setArmy(BattleSide side, const Army & army);
	void replaceBattleSetup(BattleStart & pack) const;
	void onBattleStarted(const BattleID & battleID);
	SideHealth healthOf(const BattleInfo & battle, BattleSide side) const;
};

DuelServer::DuelServer(const std::string & battleAI, int experienceRank, bool heroSpells, BattleLimits limits)
	: experienceRank(experienceRank)
	, battleAI(battleAI)
	, heroSpells(heroSpells)
	, limits(limits)
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
	// casting is a contribution of the hero, so it is only allowed when heroes are measured
	preferences.enableSpellsUsage = heroSpells;
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

void DuelServer::removeArtifacts(CGHeroInstance * hero)
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
}

void DuelServer::removeBattleParticipants()
{
	for(auto * hero : game->heroes)
	{
		removeArtifacts(hero);

		if(auto * commander = hero->getCommander())
			commander->setAlive(false);

		hero->mana = 0;
	}
}

void DuelServer::prepareHero(BattleSide side, const HeroBuild * build)
{
	CGHeroInstance * hero = game->heroes[side];
	auto & map = game->gameState->getMap();

	applyHeroType(side, build ? build->heroType : HeroTypeID::NONE);
	applyBuildingBonuses(side, build);
	applyCommander(side, build);

	if(!build && hero->secSkills.empty() && hero->level == 1 && hero->artifactsWorn.empty() && hero->getSpellsInSpellbook().empty())
		return; // already stripped, which is what every creature duel has

	removeArtifacts(hero);

	for(size_t index = 0; index < LIBRARY->skillh->size(); ++index)
		if(hero->getSecSkillLevel(SecondarySkill(static_cast<int32_t>(index))) > 0)
			hero->setSecSkillLevel(SecondarySkill(static_cast<int32_t>(index)), 0, ChangeValueMode::ABSOLUTE);

	const std::array<PrimarySkill, 4> primarySkills = {PrimarySkill::ATTACK, PrimarySkill::DEFENSE, PrimarySkill::SPELL_POWER, PrimarySkill::KNOWLEDGE};
	for(size_t index = 0; index < primarySkills.size(); ++index)
		hero->setPrimarySkill(primarySkills[index], build ? build->primary[index] : 0, ChangeValueMode::ABSOLUTE);

	hero->level = build ? std::max(1, build->level) : 1;

	if(build)
	{
		for(const auto & [skill, level] : build->skills)
			hero->setSecSkillLevel(skill, std::clamp(level, 1, 3), ChangeValueMode::ABSOLUTE);

		std::vector<ArtifactID> artifacts = build->warMachines;
		if(build->spellbook)
			artifacts.push_back(ArtifactID(ArtifactID::SPELLBOOK));

		for(const auto & artifact : artifacts)
		{
			const auto position = ArtifactUtils::getArtEquippedPosition(hero, artifact);
			if(position == ArtifactPosition::PRE_FIRST)
			{
				logGlobal->warn("Creature duels: hero has no free slot for artifact %s", artifact.toArtifact()->getJsonKey());
				continue;
			}
			auto * instance = game->gameState->createArtifact(artifact);
			map.putArtifactInstance(*hero, instance->getId(), position);
		}
	}

	// after the skills, as wisdom decides which spells the hero can learn
	applySpells(side, build);

	hero->mana = build ? hero->manaLimit() : 0;
}

void DuelServer::applyHeroType(BattleSide side, HeroTypeID type)
{
	CGHeroInstance * hero = game->heroes[side];
	if(appliedHeroType[side] == type)
		return;

	if(appliedHeroType[side] != HeroTypeID::NONE)
		for(const auto & bonus : appliedHeroType[side].toHeroType()->specialty)
			hero->removeBonus(bonus);

	appliedHeroType[side] = type;
	if(type == HeroTypeID::NONE)
		return;

	hero->setHeroType(type);
	for(const auto & bonus : type.toHeroType()->specialty)
		hero->addNewBonus(bonus);
}

void DuelServer::applyBuildingBonuses(BattleSide side, const HeroBuild * build)
{
	CGHeroInstance * hero = game->heroes[side];
	for(const auto & bonus : addedBuildingBonuses[side])
		hero->removeBonus(bonus);
	addedBuildingBonuses[side].clear();

	if(!build)
		return;

	// bonuses a town building gives all units of its player reach the army through its hero
	for(const CBuilding * building : build->buildings)
	{
		for(const auto & bonus : building->buildingBonuses)
		{
			if(!bonus->propagator || bonus->propagator->getPropagatorType() != BonusNodeType::PLAYER)
				continue;

			auto copy = std::make_shared<Bonus>(*bonus);
			copy->propagator.reset();
			hero->addNewBonus(copy);
			addedBuildingBonuses[side].push_back(copy);
		}
	}
}

void DuelServer::applyCommander(BattleSide side, const HeroBuild * build)
{
	CGHeroInstance * hero = game->heroes[side];
	const bool wanted = build && build->commander
		&& game->gameState->getSettings().getBoolean(EGameSettings::MODULE_COMMANDERS)
		&& hero->getHeroClass()->commander.hasValue();

	if(!wanted)
	{
		if(hero->getCommander())
			hero->getCommander()->setAlive(false);
		return;
	}

	auto commander = std::make_unique<CCommanderInstance>(game->gameState.get(), hero->getHeroClass()->commander);
	commander->setArmy(hero);
	CRandomGenerator random(static_cast<int>(build->commanderSeed));
	const uint32_t level = static_cast<uint32_t>(std::max(1, build->level));
	while(commander->level < level)
		commander->levelUpAutomatically(random);
	hero->replaceCommander(std::move(commander));
}

void DuelServer::applySpells(BattleSide side, const HeroBuild * build)
{
	CGHeroInstance * hero = game->heroes[side];
	const std::set<SpellID> known = hero->getSpellsInSpellbook();
	for(const auto & spell : known)
		hero->removeSpellFromSpellbook(spell);

	if(!build || build->heroType == HeroTypeID::NONE)
		return;

	for(const auto & spell : hero->getHeroType()->spells)
		hero->addSpellToSpellbook(spell);

	for(const auto & spell : build->spells)
		if(hero->canLearnSpell(spell.toSpell()))
			hero->addSpellToSpellbook(spell);
}

void DuelServer::setArmy(BattleSide side, const Army & army)
{
	CGHeroInstance * hero = game->heroes[side];
	hero->clearSlots();

	if(army.size() > GameConstants::ARMY_SIZE)
		throw std::runtime_error("Army has more stacks than a hero can lead");

	for(size_t index = 0; index < army.size(); ++index)
	{
		const CCreature * creature = army[index].creature;
		const int count = army[index].count;
		auto stack = std::make_unique<CStackInstance>(game->gameState.get(), creature->getId(), count);

		const int wantedRank = sideRanks[side] >= 0 ? sideRanks[side] : experienceRank;
		if(wantedRank > 0)
		{
			const auto & ranks = LIBRARY->creh->expRanks.at(vstd::iswithin(creature->getLevel(), 1, 7) ? creature->getLevel() : 0);
			const int rank = std::clamp<int>(wantedRank, 1, static_cast<int>(ranks.size()) - 1);
			stack->giveTotalStackExperience(static_cast<TExpType>(ranks.at(rank - 1)) * count);
		}

		hero->putStack(SlotID(static_cast<int32_t>(index)), std::move(stack));
	}
}

BattleOutcome DuelServer::play(const CCreature * left, int leftCount, const CCreature * right, int rightCount, int seed)
{
	return play({{left, leftCount}}, nullptr, {{right, rightCount}}, nullptr, seed);
}

BattleOutcome DuelServer::play(const Army & attackerArmy, const HeroBuild * attackerHero, const Army & defenderArmy, const HeroBuild * defenderHero, int seed)
{
	prepareHero(BattleSide::ATTACKER, attackerHero);
	prepareHero(BattleSide::DEFENDER, defenderHero);
	setArmy(BattleSide::ATTACKER, attackerArmy);
	setArmy(BattleSide::DEFENDER, defenderArmy);

	heroSpellsCast = {0, 0};
	describingBattle = describeNextBattle;
	if(describeNextBattle)
	{
		describeNextBattle = false;
		for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		{
			const CGHeroInstance * hero = game->heroes[side];
			const CCommanderInstance * commander = hero->getCommander();
			logGlobal->info("Faction armies: %s hero %s (%s) level %d, %d/%d/%d/%d, %d skills, %d spells, %d specialty and %d building bonuses, commander %s",
				side == BattleSide::ATTACKER ? "attacker" : "defender",
				hero->getHeroType()->getJsonKey(), hero->getHeroClass()->getJsonKey(), static_cast<int>(hero->level),
				hero->getPrimSkillLevel(PrimarySkill::ATTACK), hero->getPrimSkillLevel(PrimarySkill::DEFENSE),
				hero->getPrimSkillLevel(PrimarySkill::SPELL_POWER), hero->getPrimSkillLevel(PrimarySkill::KNOWLEDGE),
				static_cast<int>(hero->secSkills.size()), static_cast<int>(hero->getSpellsInSpellbook().size()),
				static_cast<int>(hero->getHeroType()->specialty.size()), static_cast<int>(addedBuildingBonuses[side].size()),
				commander && commander->alive ? commander->getCreature()->getJsonKey() + " level " + std::to_string(commander->level) : std::string("none"));
		}
	}

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
		if(battle->getRound() > limits.rounds || actions > limits.actions || std::chrono::steady_clock::now() - startTime > limits.duration)
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

	if(describingBattle)
	{
		int commanders = 0;
		for(const auto & stack : battle->stacks)
			if(stack->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER)
				++commanders;
		logGlobal->info("Faction armies: first battle took %d rounds, hero spells cast %d by attacker and %d by defender, %d commanders took part",
			battle->getRound(), heroSpellsCast[BattleSide::ATTACKER], heroSpellsCast[BattleSide::DEFENDER], commanders);
		describingBattle = false;
	}
	for(auto side : {BattleSide::ATTACKER, BattleSide::DEFENDER})
		outcome.health[side] = healthOf(*battle, side);

	if(finished)
		finished(*battle, outcome);

	handler->battles->discardBattle(currentBattle);
	return outcome;
}

SideHealth DuelServer::healthOf(const BattleInfo & battle, BattleSide side) const
{
	SideHealth result;

	for(const auto & stack : battle.stacks)
	{
		// war machines are not part of the army that is measured
		// neither are war machines and the commander, which come with the hero
		if(stack->unitSide() != side || stack->summoned || stack->isClone() || stack->unitSlot() == SlotID::WAR_MACHINES_SLOT || stack->unitSlot() == SlotID::COMMANDER_SLOT_PLACEHOLDER)
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
	const auto terrain = this->terrain ? *this->terrain : game->gameState->getTile(proposed.tile)->getTerrainID();

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
	else if(auto * activation = dynamic_cast<BattleSetActiveStack *>(&pack); activation && activation->reason != BattleUnitTurnReason::AUTOMATIC_ACTION)
	{
		const BattleInfo * battle = game->gameState->getBattle(activation->battleID);
		const CStack * stack = battle->battleGetStackByID(activation->stack);

		PendingEvent event;
		event.battleID = activation->battleID;
		event.unit = static_cast<int32_t>(activation->stack);
		// player of the other side, if the unit is hypnotized
		event.player = battle->battleGetOwner(stack);
		pending.push_back(event);
	}
	else if(auto * started = dynamic_cast<StartAction *>(&pack))
	{
		currentAction = started->ba;
		if(started->ba.actionType == EActionType::HERO_SPELL)
			++heroSpellsCast[started->ba.side];
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

	if(observer && currentBattle != BattleID::NONE)
		if(const BattleInfo * battle = game->gameState->getBattle(currentBattle))
			observer(pack, *battle);
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

/// Searches the size of the reference side that the subject side is even against. Each battle
/// is played by `play`, which fields the reference side at the guessed size and returns the size
/// it actually fielded, as fielding only whole units rounds the guess
struct SearchSettings
{
	double start = 1;
	double lowest = 1;
	double highest = 1;
	uint32_t seedBase = 0;
};

using PlayAtSize = std::function<BattleOutcome(double guess, bool subjectAttacks, int seed, double & fielded)>;

JobResult searchEquilibrium(const SearchSettings & settings, const PlayAtSize & play)
{
	JobResult result;

	double guess = std::clamp(settings.start, settings.lowest, settings.highest);
	std::vector<double> previousStep;
	std::vector<double> lastStep;

	for(int step = 0; step < maxSteps; ++step)
	{
		std::vector<double> estimates;
		double fielded = guess;

		for(int index = 0; index < battlesPerStep; ++index)
		{
			// the subject alternates between sides, so that neither side's advantage stays with it
			const bool subjectAttacks = (result.battles % 2) == 0;
			const int seed = static_cast<int>(settings.seedBase + result.battles);
			++result.battles;

			const BattleOutcome outcome = play(guess, subjectAttacks, seed, fielded);

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
			estimates.push_back(std::log(fielded) + 0.5 * std::log(strengthRatio));
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
		next = std::clamp(next, fielded / maxStepFactor, fielded * maxStepFactor);

		if(next < settings.lowest || next > settings.highest)
		{
			result.status = "out-of-range";
			next = std::clamp(next, settings.lowest, settings.highest);
		}

		const bool converged = std::max(next / fielded, fielded / next) < convergedStep;
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
	const double subjectValue = configuredValue(job.subject);
	const double referenceValue = configuredValue(job.reference);

	const int subjectUnits = std::clamp(static_cast<int>(std::lround(armyBudget / subjectValue)), minSubjectUnits, maxUnits);
	const double startingUnits = subjectUnits * subjectValue / referenceValue;

	SearchSettings settings;
	settings.start = startingUnits;
	settings.lowest = std::max(1.0, startingUnits / maxPriorFactor);
	settings.highest = std::min<double>(maxUnits, startingUnits * maxPriorFactor);
	settings.seedBase = stableHash(job.key(options.experienceRank));

	JobResult result = searchEquilibrium(settings, [&](double guess, bool subjectAttacks, int seed, double & fielded)
	{
		const int referenceUnits = std::max(1, static_cast<int>(std::lround(guess)));
		fielded = referenceUnits;
		return subjectAttacks
			? server->play(job.subject, subjectUnits, job.reference, referenceUnits, seed)
			: server->play(job.reference, referenceUnits, job.subject, subjectUnits, seed);
	});

	result.subjectUnits = subjectUnits;
	result.startingUnits = startingUnits;
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

/// One hero build of one faction, measured with the faction's army against the same army led by
/// a stripped hero
struct FactionJob
{
	std::string faction;
	std::string name;
	Army army;
	HeroBuild build;
	std::string error;

	std::string key() const
	{
		return faction + "," + name;
	}
};

class FactionRunner
{
public:
	explicit FactionRunner(const CreatureDuels::Options & options);
	void run();

private:
	const CreatureDuels::Options & options;
	std::unique_ptr<DuelServer> server;

	std::vector<FactionJob> jobs;
	std::set<std::string> done;

	static DuelServer * createServer(const CreatureDuels::Options & options);
	boost::filesystem::path markerPath() const;
	void collectJobs();
	void loadDone();
	void recoverCrashedJob();
	JobResult measure(const FactionJob & job);
	void writeResult(const FactionJob & job, const JobResult & result, double seconds);
};

FactionRunner::FactionRunner(const CreatureDuels::Options & options)
	: options(options)
{
}

DuelServer * FactionRunner::createServer(const CreatureDuels::Options & options)
{
	BattleLimits limits;
	limits.actions = 4000;
	limits.duration = std::chrono::seconds(300);
	return new DuelServer(options.battleAI, 0, true, limits);
}

boost::filesystem::path FactionRunner::markerPath() const
{
	return boost::filesystem::path(options.output.string() + ".current");
}

void FactionRunner::collectJobs()
{
	std::ifstream file(options.factionBuilds.c_str(), std::ios::binary);
	if(!file)
		throw std::runtime_error("Unable to open faction hero builds " + options.factionBuilds.string());
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const JsonNode root(reinterpret_cast<const std::byte *>(text.data()), text.size(), options.factionBuilds.string());

	std::map<std::string, const CCreature *> creatures;
	for(const auto & creature : LIBRARY->creh->objects)
		if(creature)
			creatures[creature->getJsonKey()] = creature.get();

	std::map<std::string, ArtifactID> artifacts;
	for(const auto & artifact : LIBRARY->arth->objects)
		if(artifact)
			artifacts[artifact->getJsonKey()] = artifact->getId();

	std::map<std::string, SecondarySkill> skills;
	for(size_t index = 0; index < LIBRARY->skillh->size(); ++index)
	{
		const SecondarySkill skill(static_cast<int32_t>(index));
		skills[skill.toSkill()->getJsonKey()] = skill;
	}

	int jobIndex = 0;
	for(const auto & factionNode : root["factions"].Vector())
	{
		Army army;
		std::string armyError;
		for(const auto & stackNode : factionNode["army"].Vector())
		{
			const auto found = creatures.find(stackNode[0].String());
			if(found == creatures.end())
				armyError += "unknown creature " + stackNode[0].String() + "; ";
			else
				army.push_back({found->second, static_cast<int>(stackNode[1].Integer())});
		}

		for(const auto & buildNode : factionNode["builds"].Vector())
		{
			if(jobIndex++ % options.shardCount != options.shardIndex)
				continue;

			FactionJob job;
			job.faction = factionNode["faction"].String();
			job.name = buildNode["name"].String();
			job.army = army;
			job.error = armyError;

			job.build.level = static_cast<int>(buildNode["level"].Integer());
			for(size_t index = 0; index < job.build.primary.size(); ++index)
				job.build.primary[index] = static_cast<int>(buildNode["primary"][index].Integer());
			for(const auto & skillNode : buildNode["skills"].Vector())
			{
				const auto found = skills.find(skillNode[0].String());
				if(found == skills.end())
					job.error += "unknown skill " + skillNode[0].String() + "; ";
				else
					job.build.skills.emplace_back(found->second, static_cast<int>(skillNode[1].Integer()));
			}
			for(const auto & machineNode : buildNode["warMachines"].Vector())
			{
				const auto found = artifacts.find(machineNode.String());
				if(found == artifacts.end())
					job.error += "unknown war machine " + machineNode.String() + "; ";
				else
					job.build.warMachines.push_back(found->second);
			}
			job.build.spellbook = buildNode["spellbook"].Bool();

			if(army.empty())
				job.error += "empty army; ";
			jobs.push_back(std::move(job));
		}
	}
}

void FactionRunner::loadDone()
{
	std::ifstream file(options.output.c_str());
	std::string line;
	while(std::getline(file, line))
	{
		std::vector<std::string> fields;
		boost::split(fields, line, boost::is_any_of(","));
		if(fields.size() > 2)
			done.insert(fields[0] + "," + fields[1]);
	}
}

void FactionRunner::recoverCrashedJob()
{
	std::ifstream marker(markerPath().c_str());
	std::string key;
	if(!std::getline(marker, key) || key.empty() || done.count(key))
		return;

	for(const auto & job : jobs)
	{
		if(job.key() != key)
			continue;

		JobResult result;
		result.status = "crashed";
		writeResult(job, result, 0);
		done.insert(key);
		logGlobal->warn("Faction heroes: %s crashed the previous run and is skipped", key);
	}
}

JobResult FactionRunner::measure(const FactionJob & job)
{
	double armyValue = 0;
	for(const auto & stack : job.army)
		armyValue += configuredValue(stack.creature) * stack.count;

	SearchSettings settings;
	settings.start = 1.0;
	settings.lowest = 1.0 / maxStepFactor;
	settings.highest = maxStepFactor;
	settings.seedBase = stableHash(job.key());

	// the reference side is the same army scaled, led by a stripped hero, so that the scale it is
	// even at is what the hero is worth, as a factor on the army
	return searchEquilibrium(settings, [&](double guess, bool subjectAttacks, int seed, double & fielded)
	{
		Army scaled;
		double scaledValue = 0;
		for(const auto & stack : job.army)
		{
			const int count = std::max(1, static_cast<int>(std::lround(stack.count * guess)));
			scaled.push_back({stack.creature, count});
			scaledValue += configuredValue(stack.creature) * count;
		}
		fielded = scaledValue / armyValue;

		return subjectAttacks
			? server->play(job.army, &job.build, scaled, nullptr, seed)
			: server->play(scaled, nullptr, job.army, &job.build, seed);
	});
}

void FactionRunner::writeResult(const FactionJob & job, const JobResult & result, double seconds)
{
	const bool newFile = !boost::filesystem::exists(options.output);
	std::ofstream file(options.output.c_str(), std::ios::app);

	if(newFile)
		file << "faction,build,level,equilibriumScale,spread,battles,valid,limited,rejected,steps,seconds,status\n";

	file << boost::str(boost::format("%s,%d,%.4f,%.4f,%d,%d,%d,%d,%d,%.1f,%s\n")
		% job.key()
		% job.build.level
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

void FactionRunner::run()
{
	collectJobs();
	loadDone();
	recoverCrashedJob();

	const size_t remaining = std::count_if(jobs.begin(), jobs.end(), [this](const FactionJob & job) { return !done.count(job.key()); });

	{
		std::ofstream total(boost::filesystem::path(options.output.string() + ".jobs").c_str());
		total << jobs.size() << "\n";
	}

	logGlobal->info("Faction heroes: shard %d of %d, %d builds, %d still to measure",
		options.shardIndex + 1, options.shardCount, static_cast<int>(jobs.size()), static_cast<int>(remaining));

	server.reset(createServer(options));

	const auto startTime = std::chrono::steady_clock::now();
	size_t measured = 0;

	for(const auto & job : jobs)
	{
		const std::string key = job.key();
		if(done.count(key))
			continue;

		{
			std::ofstream marker(markerPath().c_str());
			marker << key << "\n";
		}

		const auto jobStart = std::chrono::steady_clock::now();
		JobResult result;

		if(!job.error.empty())
		{
			result.status = "error: " + job.error;
			boost::replace_all(result.status, ",", ";");
			logGlobal->error("Faction heroes: %s skipped: %s", key, job.error);
		}
		else
		{
			try
			{
				result = measure(job);
			}
			catch(const std::exception & e)
			{
				result.status = std::string("error: ") + e.what();
				boost::replace_all(result.status, ",", ";");
				boost::replace_all(result.status, "\n", " ");
				logGlobal->error("Faction heroes: %s failed: %s", key, e.what());

				server.reset();
				server.reset(createServer(options));
			}
		}

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - jobStart).count();
		writeResult(job, result, seconds);
		done.insert(key);
		++measured;

		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
		const double eta = elapsed / measured * (remaining - measured);
		logGlobal->info("Faction heroes: %d/%d %s - %s, %.0f s left", static_cast<int>(measured), static_cast<int>(remaining), key, result.status, eta);
	}

	boost::filesystem::remove(markerPath());
	logGlobal->info("Faction heroes complete");
}


/// One side of a faction army battle: the army a faction raised by some day, and the hero leading it
struct ArmySide
{
	std::string id;
	std::string faction;
	Army army;
	HeroBuild build;
	int experienceRank = 0;
	std::string error;
};

/// Two sides of faction army battles: the second side's army is scaled until the two are even
struct ArmyJob
{
	size_t first = 0;
	size_t second = 0;
	std::string key;
};

/// Identifiers of everything a side of a faction army battle or a battle test can name
struct SideLookups
{
	std::map<std::string, const CCreature *> creatures;
	std::map<std::string, ArtifactID> artifacts;
	std::map<std::string, SecondarySkill> skills;
	std::map<std::string, HeroTypeID> heroTypes;
	std::map<std::string, SpellID> spells;
	std::map<std::string, const CFaction *> factions;

	SideLookups();
};

SideLookups::SideLookups()
{
	for(const auto & creature : LIBRARY->creh->objects)
		if(creature)
			creatures[creature->getJsonKey()] = creature.get();

	for(const auto & artifact : LIBRARY->arth->objects)
		if(artifact)
			artifacts[artifact->getJsonKey()] = artifact->getId();

	for(size_t index = 0; index < LIBRARY->skillh->size(); ++index)
	{
		const SecondarySkill skill(static_cast<int32_t>(index));
		skills[skill.toSkill()->getJsonKey()] = skill;
	}

	for(const auto & hero : LIBRARY->heroh->objects)
		if(hero)
			heroTypes[hero->getJsonKey()] = hero->getId();

	for(const auto & spell : LIBRARY->spellh->objects)
		if(spell)
			spells[spell->getJsonKey()] = spell->getId();

	for(const auto & faction : LIBRARY->townh->objects)
		if(faction)
			factions[faction->getJsonKey()] = faction.get();
}

/// Side of a faction army battle or a battle test, as written in their JSON files; what is wrong
/// with it goes to its error, so that one bad side does not stop the others
ArmySide parseArmySide(const JsonNode & sideNode, const SideLookups & lookups)
{
	ArmySide side;
	side.id = sideNode["id"].String();
	side.faction = sideNode["faction"].String();
	side.experienceRank = static_cast<int>(sideNode["experienceRank"].Integer());

	for(const auto & stackNode : sideNode["army"].Vector())
	{
		const auto found = lookups.creatures.find(stackNode[0].String());
		if(found == lookups.creatures.end())
			side.error += "unknown creature " + stackNode[0].String() + "; ";
		else
			side.army.push_back({found->second, static_cast<int>(stackNode[1].Integer())});
	}
	if(side.army.empty())
		side.error += "empty army; ";
	if(side.army.size() > GameConstants::ARMY_SIZE)
		side.error += "more stacks than a hero can lead; ";

	HeroBuild & build = side.build;
	build.level = static_cast<int>(sideNode["level"].Integer());
	for(size_t index = 0; index < build.primary.size(); ++index)
		build.primary[index] = static_cast<int>(sideNode["primary"][index].Integer());

	const auto hero = lookups.heroTypes.find(sideNode["hero"].String());
	if(hero == lookups.heroTypes.end())
		side.error += "unknown hero " + sideNode["hero"].String() + "; ";
	else
		build.heroType = hero->second;

	for(const auto & skillNode : sideNode["skills"].Vector())
	{
		const auto found = lookups.skills.find(skillNode[0].String());
		if(found == lookups.skills.end())
			side.error += "unknown skill " + skillNode[0].String() + "; ";
		else
			build.skills.emplace_back(found->second, static_cast<int>(skillNode[1].Integer()));
	}
	for(const auto & machineNode : sideNode["warMachines"].Vector())
	{
		const auto found = lookups.artifacts.find(machineNode.String());
		if(found == lookups.artifacts.end())
			side.error += "unknown war machine " + machineNode.String() + "; ";
		else
			build.warMachines.push_back(found->second);
	}
	for(const auto & spellNode : sideNode["spells"].Vector())
	{
		// a spell a mod no longer has is only left out, it does not stop the side
		const auto found = lookups.spells.find(spellNode.String());
		if(found != lookups.spells.end())
			build.spells.push_back(found->second);
		else
			logGlobal->warn("Faction armies: %s: unknown spell %s", side.id, spellNode.String());
	}

	const auto faction = lookups.factions.find(side.faction);
	if(side.faction.empty())
	{
		if(!sideNode["buildings"].Vector().empty())
			side.error += "buildings need a faction; ";
	}
	else if(faction == lookups.factions.end() || !faction->second->town)
		side.error += "unknown faction " + side.faction + "; ";
	else
	{
		for(const auto & buildingNode : sideNode["buildings"].Vector())
		{
			const std::string & name = buildingNode.String();
			const CBuilding * match = nullptr;
			for(const auto & [id, building] : faction->second->town->buildings)
			{
				const std::string key = building->getJsonKey();
				if(key == name || boost::algorithm::ends_with(key, ":" + name))
					match = building.get();
			}
			if(match)
				build.buildings.push_back(match);
			else
				logGlobal->warn("Faction armies: %s: town has no building %s", side.id, name);
		}
	}

	build.spellbook = sideNode["spellbook"].Bool();
	build.commander = sideNode["commander"].Bool();
	build.commanderSeed = stableHash(side.id);
	return side;
}

class ArmyRunner
{
public:
	explicit ArmyRunner(const CreatureDuels::Options & options);
	void run();

private:
	const CreatureDuels::Options & options;
	std::unique_ptr<DuelServer> server;

	std::vector<ArmySide> sides;
	std::vector<ArmyJob> jobs;
	std::set<std::string> done;

	static DuelServer * createServer(const CreatureDuels::Options & options);
	boost::filesystem::path markerPath() const;
	void collectJobs();
	void loadDone();
	void recoverCrashedJob();
	JobResult measure(const ArmyJob & job);
	void writeResult(const ArmyJob & job, const JobResult & result, double seconds);
};

ArmyRunner::ArmyRunner(const CreatureDuels::Options & options)
	: options(options)
{
}

DuelServer * ArmyRunner::createServer(const CreatureDuels::Options & options)
{
	BattleLimits limits;
	limits.actions = 5000;
	limits.duration = std::chrono::seconds(300);
	return new DuelServer(options.battleAI, 0, true, limits);
}

boost::filesystem::path ArmyRunner::markerPath() const
{
	return boost::filesystem::path(options.output.string() + ".current");
}

void ArmyRunner::collectJobs()
{
	std::ifstream file(options.armyJobs.c_str(), std::ios::binary);
	if(!file)
		throw std::runtime_error("Unable to open faction army jobs " + options.armyJobs.string());
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const JsonNode root(reinterpret_cast<const std::byte *>(text.data()), text.size(), options.armyJobs.string());

	const SideLookups lookups;

	std::map<std::string, size_t> sideIndex;
	for(const auto & sideNode : root["sides"].Vector())
	{
		ArmySide side = parseArmySide(sideNode, lookups);
		sideIndex[side.id] = sides.size();
		sides.push_back(std::move(side));
	}

	int jobIndex = 0;
	for(const auto & pairNode : root["pairs"].Vector())
	{
		if(jobIndex++ % options.shardCount != options.shardIndex)
			continue;

		const auto first = sideIndex.find(pairNode[0].String());
		const auto second = sideIndex.find(pairNode[1].String());
		if(first == sideIndex.end() || second == sideIndex.end())
		{
			logGlobal->error("Faction armies: pair %s, %s names an unknown side", pairNode[0].String(), pairNode[1].String());
			continue;
		}

		ArmyJob job;
		job.first = first->second;
		job.second = second->second;
		job.key = sides[job.first].id + "," + sides[job.second].id;
		jobs.push_back(job);
	}
}

void ArmyRunner::loadDone()
{
	std::ifstream file(options.output.c_str());
	std::string line;
	while(std::getline(file, line))
	{
		std::vector<std::string> fields;
		boost::split(fields, line, boost::is_any_of(","));
		if(fields.size() > 2)
			done.insert(fields[0] + "," + fields[1]);
	}
}

void ArmyRunner::recoverCrashedJob()
{
	std::ifstream marker(markerPath().c_str());
	std::string key;
	if(!std::getline(marker, key) || key.empty() || done.count(key))
		return;

	for(const auto & job : jobs)
	{
		if(job.key != key)
			continue;

		JobResult result;
		result.status = "crashed";
		writeResult(job, result, 0);
		done.insert(key);
		logGlobal->warn("Faction armies: %s crashed the previous run and is skipped", key);
	}
}

JobResult ArmyRunner::measure(const ArmyJob & job)
{
	const ArmySide & first = sides[job.first];
	const ArmySide & second = sides[job.second];

	double firstValue = 0;
	for(const auto & stack : first.army)
		firstValue += configuredValue(stack.creature) * stack.count;
	double secondValue = 0;
	for(const auto & stack : second.army)
		secondValue += configuredValue(stack.creature) * stack.count;

	// configured values suggest where the two are even, which saves search steps
	const double prior = std::clamp(firstValue / std::max(1.0, secondValue), 1.0 / 8.0, 8.0);

	SearchSettings settings;
	settings.start = prior;
	settings.lowest = prior / 6.0;
	settings.highest = prior * 6.0;
	settings.seedBase = stableHash(job.key);
	server->describeNextBattle = true;

	// the second side's army is scaled, its hero stays, so the scale it is even at is how much
	// stronger the first side is, as a factor on the second side's army
	return searchEquilibrium(settings, [&](double guess, bool subjectAttacks, int seed, double & fielded)
	{
		Army scaled;
		double scaledValue = 0;
		for(const auto & stack : second.army)
		{
			const int count = std::max(1, static_cast<int>(std::lround(stack.count * guess)));
			scaled.push_back({stack.creature, count});
			scaledValue += configuredValue(stack.creature) * count;
		}
		fielded = scaledValue / std::max(1.0, secondValue);

		if(subjectAttacks)
		{
			server->setExperienceRanks(first.experienceRank, second.experienceRank);
			return server->play(first.army, &first.build, scaled, &second.build, seed);
		}

		server->setExperienceRanks(second.experienceRank, first.experienceRank);
		return server->play(scaled, &second.build, first.army, &first.build, seed);
	});
}

void ArmyRunner::writeResult(const ArmyJob & job, const JobResult & result, double seconds)
{
	const bool newFile = !boost::filesystem::exists(options.output);
	std::ofstream file(options.output.c_str(), std::ios::app);

	if(newFile)
		file << "first,second,equilibriumScale,spread,battles,valid,limited,rejected,steps,seconds,status\n";

	file << boost::str(boost::format("%s,%.4f,%.4f,%d,%d,%d,%d,%d,%.1f,%s\n")
		% job.key
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

void ArmyRunner::run()
{
	collectJobs();
	loadDone();
	recoverCrashedJob();

	const size_t remaining = std::count_if(jobs.begin(), jobs.end(), [this](const ArmyJob & job) { return !done.count(job.key); });

	{
		std::ofstream total(boost::filesystem::path(options.output.string() + ".jobs").c_str());
		total << jobs.size() << "\n";
	}

	logGlobal->info("Faction armies: shard %d of %d, %d pairs, %d still to measure",
		options.shardIndex + 1, options.shardCount, static_cast<int>(jobs.size()), static_cast<int>(remaining));

	server.reset(createServer(options));

	const auto startTime = std::chrono::steady_clock::now();
	size_t measured = 0;

	for(const auto & job : jobs)
	{
		if(done.count(job.key))
			continue;

		{
			std::ofstream marker(markerPath().c_str());
			marker << job.key << "\n";
		}

		const auto jobStart = std::chrono::steady_clock::now();
		JobResult result;
		const std::string error = sides[job.first].error + sides[job.second].error;

		if(!error.empty())
		{
			result.status = "error: " + error;
			boost::replace_all(result.status, ",", ";");
			logGlobal->error("Faction armies: %s skipped: %s", job.key, error);
		}
		else
		{
			try
			{
				result = measure(job);
			}
			catch(const std::exception & e)
			{
				result.status = std::string("error: ") + e.what();
				boost::replace_all(result.status, ",", ";");
				boost::replace_all(result.status, "\n", " ");
				logGlobal->error("Faction armies: %s failed: %s", job.key, e.what());

				server.reset();
				server.reset(createServer(options));
			}
		}

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - jobStart).count();
		writeResult(job, result, seconds);
		done.insert(job.key);
		++measured;

		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
		const double eta = elapsed / measured * (remaining - measured);
		logGlobal->info("Faction armies: %d/%d %s - %s, %.0f s left", static_cast<int>(measured), static_cast<int>(remaining), job.key, result.status, eta);
	}

	boost::filesystem::remove(markerPath());
	logGlobal->info("Faction armies complete");
}

/// Plays the battles a JSON file describes once each and writes down everything that happens in
/// them: actions, damage with friendly fire marked, and every change to each unit's count, position
/// and bonuses. For checking abilities and combat scripts without starting the game
class BattleTestRunner
{
public:
	explicit BattleTestRunner(const CreatureDuels::Options & options)
		: options(options)
	{
	}

	void run();

private:
	struct UnitSnapshot
	{
		bool alive = false;
		int count = 0;
		int firstHP = 0;
		int position = -1;
		std::set<std::string> bonuses;
	};

	const CreatureDuels::Options & options;
	std::ofstream out;
	std::map<uint32_t, UnitSnapshot> snapshots;
	/// Unit whose action or spell is being carried out, for damage that names no attacker
	uint32_t actor = -1;
	int damageEvents = 0;
	int friendlyFire = 0;

	static std::string unitName(const BattleInfo & battle, uint32_t id);
	static std::string bonusText(const Bonus & bonus);
	static UnitSnapshot snapshot(const CStack & stack);
	void compare(const BattleInfo & battle, bool initial);
	void observe(const CPackForClient & pack, const BattleInfo & battle);
	void logDamage(const BattleInfo & battle, const BattleStackAttacked & attack, uint32_t fallbackAttacker);
	void logEnd(const BattleInfo & battle, const BattleOutcome & outcome);
};

std::string BattleTestRunner::unitName(const BattleInfo & battle, uint32_t id)
{
	const CStack * stack = battle.battleGetStackByID(id, false);
	if(!stack)
		return "unit#" + std::to_string(id);

	const std::string side = stack->unitSide() == BattleSide::ATTACKER ? "A" : "D";
	return boost::str(boost::format("%s:%s#%d") % side % stack->unitType()->getNameSingularTranslated() % id);
}

std::string BattleTestRunner::bonusText(const Bonus & bonus)
{
	// written out by hand: limiters and updaters of some mods cannot be turned back into JSON
	std::string text = LIBRARY->bth->bonusToString(bonus.type);

	if(bonus.subtype != BonusSubtypeID())
		text += "(" + std::to_string(bonus.subtype.getNum()) + ")";
	text += " val=" + std::to_string(bonus.val);
	if(bonus.valType != BonusValueType::ADDITIVE_VALUE)
		text += " " + vstd::findKey(bonusValueMap, bonus.valType);
	if(bonus.duration != BonusDuration::PERMANENT)
		text += " duration=" + std::to_string(static_cast<int>(bonus.duration));
	if(bonus.turnsRemain)
		text += " turns=" + std::to_string(bonus.turnsRemain);
	text += " from=" + vstd::findKey(bonusSourceMap, bonus.source);
	if(bonus.sid != BonusSourceID())
		text += ":" + std::to_string(bonus.sid.getNum());
	if(!bonus.stacking.empty())
		text += " stacking=" + bonus.stacking;
	if(!bonus.description.empty())
		text += " \"" + bonus.description.toString(LIBRARY->staticTexts()) + "\"";
	if(bonus.hidden)
		text += " hidden";
	return text;
}

BattleTestRunner::UnitSnapshot BattleTestRunner::snapshot(const CStack & stack)
{
	UnitSnapshot result;
	result.alive = stack.alive();
	result.count = stack.getCount();
	result.firstHP = stack.getFirstHPleft();
	result.position = stack.getPosition().toInt();
	// kept in a variable, as the list would not outlive the loop over a temporary
	const auto bonuses = stack.getAllBonuses(Selector::all);
	for(const auto & bonus : *bonuses)
		result.bonuses.insert(bonusText(*bonus));
	return result;
}

void BattleTestRunner::compare(const BattleInfo & battle, bool initial)
{
	initial = initial || snapshots.empty();
	for(const auto & stack : battle.stacks)
	{
		const uint32_t id = stack->unitId();
		UnitSnapshot now = snapshot(*stack);
		const auto previous = snapshots.find(id);

		if(previous == snapshots.end())
		{
			out << boost::str(boost::format("  %s %s: %d x, first %d HP, hex %d, %d bonuses\n")
				% (initial ? "unit" : "APPEARS") % unitName(battle, id) % now.count % now.firstHP % now.position % now.bonuses.size());
			snapshots[id] = std::move(now);
			continue;
		}

		const UnitSnapshot & before = previous->second;
		const std::string name = unitName(battle, id);
		if(before.alive != now.alive)
			out << "  " << name << (now.alive ? " REVIVED" : " DIED") << "\n";
		if(before.count != now.count || before.firstHP != now.firstHP)
			out << boost::str(boost::format("  %s: %d x (%d HP) -> %d x (%d HP)\n") % name % before.count % before.firstHP % now.count % now.firstHP);
		if(before.position != now.position)
			out << boost::str(boost::format("  %s: hex %d -> %d\n") % name % before.position % now.position);
		for(const auto & text : before.bonuses)
			if(!now.bonuses.count(text))
				out << "  " << name << " - " << text << "\n";
		for(const auto & text : now.bonuses)
			if(!before.bonuses.count(text))
				out << "  " << name << " + " << text << "\n";

		previous->second = std::move(now);
	}
}

void BattleTestRunner::logDamage(const BattleInfo & battle, const BattleStackAttacked & attack, uint32_t fallbackAttacker)
{
	const uint32_t attacker = attack.attackerID != static_cast<ui32>(-1) ? attack.attackerID : fallbackAttacker;
	const CStack * source = battle.battleGetStackByID(attacker, false);
	const CStack * victim = battle.battleGetStackByID(attack.stackAttacked, false);
	// damage a unit deals itself is reflected, e.g. by a fire shield, and is no friendly fire
	const bool friendly = source && victim && source != victim && source->unitSide() == victim->unitSide() && attack.damageAmount > 0;

	++damageEvents;
	if(friendly)
		++friendlyFire;

	std::string spell;
	if(attack.isSpell() && attack.spellID.hasValue())
		spell = " by spell " + attack.spellID.toSpell()->getJsonKey();

	out << boost::str(boost::format("  DAMAGE %s -> %s: %d damage, %d killed%s%s%s%s\n")
		% (source ? unitName(battle, attacker) : std::string("(no unit)"))
		% unitName(battle, attack.stackAttacked)
		% attack.damageAmount % attack.killedAmount
		% spell
		% (attack.isSecondary() ? " (secondary)" : "")
		% (attack.killed() ? " (stack killed)" : "")
		% (friendly ? "   <<< FRIENDLY FIRE" : source == victim && source ? " (reflected)" : ""));
}

void BattleTestRunner::observe(const CPackForClient & pack, const BattleInfo & battle)
{
	if(dynamic_cast<const BattleStart *>(&pack))
	{
		// units are not part of the bonus system yet, they are compared from the first round on
		snapshots.clear();
		out << "Battle starts\n";
	}
	else if(dynamic_cast<const BattleNextRound *>(&pack))
	{
		compare(battle, false);
		out << boost::str(boost::format("==== Round %d ====\n") % battle.getRound());
	}
	else if(const auto * activation = dynamic_cast<const BattleSetActiveStack *>(&pack))
	{
		compare(battle, false);
		out << "-> " << unitName(battle, activation->stack) << " is active\n";
	}
	else if(const auto * started = dynamic_cast<const StartAction *>(&pack))
	{
		static const std::map<EActionType, std::string> names = {
			{EActionType::HERO_SPELL, "hero spell"}, {EActionType::WALK, "walk"}, {EActionType::WAIT, "wait"},
			{EActionType::DEFEND, "defend"}, {EActionType::WALK_AND_ATTACK, "attack"}, {EActionType::SHOOT, "shoot"},
			{EActionType::MONSTER_SPELL, "cast"}, {EActionType::BAD_MORALE, "bad morale"}, {EActionType::STACK_HEAL, "heal"},
			{EActionType::WALK_AND_CAST, "walk and cast"}, {EActionType::WALK_AND_ATTACK_NO_RETURN, "attack (no return)"},
			{EActionType::LONG_WEAPON_ATTACK, "long weapon attack"}, {EActionType::RANDOM_SPELL, "random spell"}};
		const BattleAction & action = started->ba;
		const auto name = names.find(action.actionType);
		const std::string what = name != names.end() ? name->second : "action " + std::to_string(static_cast<int>(action.actionType));
		if(action.actionType == EActionType::HERO_SPELL)
		{
			actor = -1;
			out << boost::str(boost::format("  %s hero casts %s\n") % (action.side == BattleSide::ATTACKER ? "A" : "D") % (action.spell.hasValue() ? action.spell.toSpell()->getJsonKey() : std::string("?")));
		}
		else
		{
			actor = action.stackNumber;
			out << "  " << unitName(battle, action.stackNumber) << ": " << what;
			if(action.spell.hasValue())
				out << " " << action.spell.toSpell()->getJsonKey();
			out << "\n";
		}
	}
	else if(const auto * cast = dynamic_cast<const BattleSpellCast *>(&pack))
	{
		if(cast->casterStack >= 0)
			actor = cast->casterStack;
		std::string targets;
		for(const auto id : cast->affectedCres)
			targets += " " + unitName(battle, id);
		out << boost::str(boost::format("  SPELL %s by %s%s%s\n")
			% (cast->spellID.hasValue() ? cast->spellID.toSpell()->getJsonKey() : std::string("?"))
			% (cast->casterStack >= 0 ? unitName(battle, cast->casterStack) : std::string(cast->castByHero ? "hero" : "?"))
			% (targets.empty() ? "" : " on")
			% targets);
	}
	else if(const auto * attack = dynamic_cast<const BattleAttack *>(&pack))
	{
		for(const auto & hit : attack->bsa)
			logDamage(battle, hit, attack->stackAttacking);
	}
	else if(const auto * injured = dynamic_cast<const StacksInjured *>(&pack))
	{
		for(const auto & hit : injured->stacks)
			logDamage(battle, hit, actor);
	}
	else if(const auto * message = dynamic_cast<const BattleLogMessage *>(&pack))
	{
		for(const auto & line : message->lines)
			out << "  LOG " << line.toString(LIBRARY->staticTexts()) << "\n";
	}
	else if(const auto * moved = dynamic_cast<const BattleStackMoved *>(&pack))
	{
		if(moved->teleporting)
			out << "  " << unitName(battle, moved->stack) << " teleported\n";
	}
	else if(dynamic_cast<const EndAction *>(&pack))
	{
		compare(battle, false);
	}
}

void BattleTestRunner::logEnd(const BattleInfo & battle, const BattleOutcome & outcome)
{
	compare(battle, false);
	out << boost::str(boost::format("Battle over after %d rounds%s: attacker keeps %.1f%% of its health, defender %.1f%%\n")
		% outcome.rounds % (outcome.limited ? " (limit reached)" : "")
		% (outcome.health[BattleSide::ATTACKER].shareLeft() * 100) % (outcome.health[BattleSide::DEFENDER].shareLeft() * 100));
	out << boost::str(boost::format("%d damage events, %d of them friendly fire, %d actions rejected\n\n") % damageEvents % friendlyFire % outcome.rejected);
}

void BattleTestRunner::run()
{
	std::ifstream file(options.battleTests.c_str(), std::ios::binary);
	if(!file)
		throw std::runtime_error("Unable to open battle tests " + options.battleTests.string());
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const JsonNode root(reinterpret_cast<const std::byte *>(text.data()), text.size(), options.battleTests.string());

	out.open(options.output.c_str());
	const SideLookups lookups;

	for(const auto & testNode : root["tests"].Vector())
	{
		const std::string name = testNode["name"].String();
		ArmySide attacker = parseArmySide(testNode["attacker"], lookups);
		ArmySide defender = parseArmySide(testNode["defender"], lookups);
		const int battles = std::max<int>(1, static_cast<int>(testNode["battles"].Integer()));

		out << "######## " << name << "\n";
		if(!attacker.error.empty() || !defender.error.empty())
		{
			out << "ERROR attacker: " << attacker.error << " defender: " << defender.error << "\n\n";
			logGlobal->error("Battle tests: %s: %s %s", name, attacker.error, defender.error);
			continue;
		}

		BattleLimits limits;
		limits.rounds = testNode["rounds"].isNull() ? 30 : static_cast<int>(testNode["rounds"].Integer());
		DuelServer server(options.battleAI, 0, !testNode["heroSpells"].isNull() ? testNode["heroSpells"].Bool() : true, limits);
		if(!testNode["terrain"].isNull())
			server.terrain = TerrainId(TerrainId::decode(testNode["terrain"].String()));
		server.observer = [this](const CPackForClient & pack, const BattleInfo & battle) { observe(pack, battle); };
		server.finished = [this](const BattleInfo & battle, const BattleOutcome & outcome) { logEnd(battle, outcome); };

		for(int index = 0; index < battles; ++index)
		{
			const int seed = static_cast<int>(testNode["seed"].Integer()) + index;
			out << "---- battle " << index + 1 << ", seed " << seed << "\n";
			damageEvents = 0;
			friendlyFire = 0;
			actor = -1;
			try
			{
				server.setExperienceRanks(attacker.experienceRank, defender.experienceRank);
				server.play(attacker.army, &attacker.build, defender.army, &defender.build, seed);
			}
			catch(const std::exception & e)
			{
				out << "ERROR " << e.what() << "\n\n";
				logGlobal->error("Battle tests: %s failed: %s", name, e.what());
			}
			out.flush();
		}
	}
	logGlobal->info("Battle tests complete, written to %s", options.output.string());
}
}

void CreatureDuels::run(const Options & options)
{
	if(options.shardCount < 1 || options.shardIndex < 0 || options.shardIndex >= options.shardCount)
		throw std::runtime_error("Invalid creature duels shard");

	if(!options.battleTests.empty())
	{
		BattleTestRunner runner(options);
		runner.run();
		return;
	}

	if(!options.armyJobs.empty())
	{
		ArmyRunner runner(options);
		runner.run();
		return;
	}

	if(!options.factionBuilds.empty())
	{
		FactionRunner runner(options);
		runner.run();
		return;
	}

	DuelRunner runner(options);
	runner.run();
}
