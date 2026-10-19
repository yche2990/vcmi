/*
 * CreatureValueEstimator.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "CreatureValueEstimator.h"
#include "BattlegroundGame.h"

#include "../lib/CCreatureHandler.h"
#include "../lib/CStack.h"
#include "../lib/GameLibrary.h"
#include "../lib/battle/BattleInfo.h"
#include "../lib/battle/BattleLayout.h"
#include "../lib/battle/CUnitState.h"
#include "../lib/battle/CombatValue.h"
#include "../lib/bonuses/Bonus.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapping/TerrainTile.h"
#include "../lib/modding/ModScope.h"
#include "../lib/networkPacks/PacksForClientBattle.h"

void CreatureValueEstimator::run(int experienceRank)
{
	CreatureValueEstimator estimator;
	estimator.experienceRank = std::max(0, experienceRank);

	estimator.startGame();
	estimator.startBattle();
	estimator.collectSubjects();
	estimator.measureDamage();
	estimator.deriveValues();
	estimator.applyFormula();
	estimator.checkModelPaths();
	estimator.report();
}

void CreatureValueEstimator::startGame()
{
	game = std::make_unique<BattlegroundGame>();
	gameState = game->gameState;
	attackerSideHero = game->heroes[BattleSide::ATTACKER];
	defenderSideHero = game->heroes[BattleSide::DEFENDER];
}

void CreatureValueEstimator::startBattle()
{
	BattleSideArray<const CGHeroInstance *> heroes = {attackerSideHero, defenderSideHero};
	BattleSideArray<const CArmedInstance *> armies = {attackerSideHero, defenderSideHero};

	const int3 tile = attackerSideHero->anchorPos();
	const auto terrain = gameState->getTile(tile)->getTerrainID();
	const BattleLayout layout = BattleLayout::createDefaultLayout(*gameState, attackerSideHero, defenderSideHero);

	const BattleField battlefield = BattlegroundGame::neutralBattlefield();

	BattleStart pack;
	pack.info = BattleInfo::setupBattle(gameState.get(), tile, terrain, battlefield, armies, heroes, layout, nullptr);
	pack.battleID = BattleID(0);
	gameState->apply(pack);

	battle()->tacticDistance = 0;
	// obstacles are placed at random, and a mine under a unit would show up as damage
	battle()->obstacles.clear();
}

BattleInfo * CreatureValueEstimator::battle() const
{
	return gameState->currentBattles.front().get();
}

void CreatureValueEstimator::collectSubjects()
{
	std::vector<const CCreature *> modded;

	for(const auto & creature : LIBRARY->creh->objects)
	{
		if(creature->special)
			continue;

		if(creature->getModScope() == ModScope::scopeBuiltin())
			subjects.push_back(creature.get());
		else
			modded.push_back(creature.get());
	}

	baselineCount = subjects.size();
	vstd::concatenate(subjects, modded);

	if(baselineCount < 100)
		throw std::runtime_error("Creatures of the original game failed to load - nothing to measure against");

	logGlobal->info("Measuring %d creatures against a baseline of %d", static_cast<int>(subjects.size()), static_cast<int>(baselineCount));
}

CStack * CreatureValueEstimator::placeStack(BattleSide side, const CCreature * creature, const BattleHex & hex)
{
	return placeStack(side, creature, hex, std::max(1, stackHitPoints / std::max(1, creature->getBaseHitPoints())));
}

CStack * CreatureValueEstimator::placeStack(BattleSide side, const CCreature * creature, const BattleHex & hex, int count)
{
	if(experienceRank > 0)
	{
		// stack experience is read from the army stack a unit comes from, so the unit needs one
		CGHeroInstance * hero = side == BattleSide::ATTACKER ? attackerSideHero : defenderSideHero;
		const SlotID slot = hero->getFreeSlot();
		if(!slot.validSlot())
			throw std::runtime_error("No free army slot to place an experienced stack in");

		const auto & ranks = LIBRARY->creh->expRanks.at(vstd::iswithin(creature->getLevel(), 1, 7) ? creature->getLevel() : 0);
		const int rank = std::clamp<int>(experienceRank, 1, static_cast<int>(ranks.size()) - 1);

		auto instance = std::make_unique<CStackInstance>(gameState.get(), creature->getId(), count);
		instance->giveTotalStackExperience(static_cast<TExpType>(ranks.at(rank - 1)) * count);
		hero->putStack(slot, std::move(instance));

		const uint32_t id = battle()->battleNextUnitId();
		battle()->generateNewStack(id, *hero->getStackPtr(slot), side, slot, hex);
		CStack * unit = battle()->stacks.back().get();
		unit->initialPosition = hex;
		unit->localInit(battle());
		experiencedStacks[id] = {hero, slot};
		return unit;
	}

	battle::UnitInfo info;
	info.id = battle()->battleNextUnitId();
	info.count = count;
	info.type = creature->getId();
	info.side = side;
	info.position = hex;
	info.summoned = false;

	BattleUnitsChanged pack;
	pack.battleID = BattleID(0);
	pack.changedStacks.emplace_back(info.id, UnitChanges::EOperation::ADD);
	info.save(pack.changedStacks.back().data);
	gameState->apply(pack);

	return battle()->getStack(info.id);
}

void CreatureValueEstimator::removeStack(const CStack * stack)
{
	BattleUnitsChanged pack;
	pack.battleID = BattleID(0);
	pack.changedStacks.emplace_back(stack->unitId(), UnitChanges::EOperation::REMOVE);
	gameState->apply(pack);

	// battle keeps removed units around so they can be raised, but this code makes far too many
	const uint32_t id = stack->unitId();
	std::erase_if(battle()->stacks, [stack](const std::unique_ptr<CStack> & entry) { return entry.get() == stack; });

	if(auto it = experiencedStacks.find(id); it != experiencedStacks.end())
	{
		it->second.first->eraseStack(it->second.second);
		experiencedStacks.erase(it);
	}
}

void CreatureValueEstimator::measurePair(const CStack * attacker, const CStack * defender, double & dealt, double & retaliated) const
{
	// jousting scales with distance covered, which is at most one turn of movement
	const int charge = std::min<int>(CombatValue::startingDistance(), attacker->getMovementRange());

	DamageEstimation retaliation;
	const DamageEstimation damage = battle()->battleEstimateDamage(attacker, defender, charge, &retaliation);

	dealt = (damage.damage.min + damage.damage.max) / 2.0 / attacker->getCount();
	retaliated = (retaliation.damage.min + retaliation.damage.max) / 2.0 / defender->getCount();
}

void CreatureValueEstimator::measureDamage()
{
	const size_t count = subjects.size();

	dealtBySubject.assign(count, std::vector<double>(baselineCount, 0.0));
	retaliatedByOpponent.assign(count, std::vector<double>(baselineCount, 0.0));
	dealtOnSubject.assign(count, std::vector<double>(baselineCount, 0.0));
	retaliatedBySubject.assign(count, std::vector<double>(baselineCount, 0.0));

	for(size_t subject = 0; subject < count; ++subject)
	{
		const CStack * attacker = placeStack(BattleSide::ATTACKER, subjects[subject], BattleHex(attackerHex));

		for(size_t opponent = 0; opponent < baselineCount; ++opponent)
		{
			const CStack * defender = placeStack(BattleSide::DEFENDER, subjects[opponent], BattleHex(defenderHex));

			measurePair(attacker, defender, dealtBySubject[subject][opponent], retaliatedByOpponent[subject][opponent]);

			// baseline creatures are attackers in their own iteration, so only modded ones need both
			if(subject >= baselineCount)
				measurePair(defender, attacker, dealtOnSubject[subject][opponent], retaliatedBySubject[subject][opponent]);

			removeStack(defender);
		}

		removeStack(attacker);
	}

	for(size_t subject = 0; subject < baselineCount; ++subject)
	{
		for(size_t opponent = 0; opponent < baselineCount; ++opponent)
		{
			dealtOnSubject[subject][opponent] = dealtBySubject[opponent][subject];
			retaliatedBySubject[subject][opponent] = retaliatedByOpponent[opponent][subject];
		}
	}
}

void CreatureValueEstimator::deriveValues()
{
	const size_t count = subjects.size();
	entries.resize(count);

	std::vector<double> rawOutput(count, 0.0);
	std::vector<double> survival(count, 1.0);

	// a live unit is used so that abilities are read through the bonus system, as in a real battle
	for(size_t subject = 0; subject < count; ++subject)
	{
		const CStack * unit = placeStack(BattleSide::ATTACKER, subjects[subject], BattleHex(attackerHex));

		const auto sum = [](const std::vector<double> & row) { return std::accumulate(row.begin(), row.end(), 0.0); };
		const double ownTurn = sum(dealtBySubject[subject]) / baselineCount * CombatValue::attacksPerRound(*unit) * CombatValue::targetsPerAttack(*unit);
		const double retaliations = sum(retaliatedBySubject[subject]) / baselineCount * CombatValue::retaliationsPerRound(*unit);

		rawOutput[subject] = std::max(ownTurn + retaliations, 1e-6);
		survival[subject] = CombatValue::survivalMultiplier(*unit) * CombatValue::situationalSurvival(*unit, values.averageBattle());

		Entry & entry = entries[subject];
		entry.creature = subjects[subject];
		entry.baseline = subject < baselineCount;
		entry.output = rawOutput[subject] * CombatValue::offenseMultiplier(*unit) * CombatValue::situationalOffense(*unit, values.averageBattle());
		entry.uptime = CombatValue::uptimeOf(*unit);

		removeStack(unit);
	}

	for(size_t subject = 0; subject < count; ++subject)
	{
		// damage taken relative to what the same attackers deal on average
		double vulnerability = 0;
		for(size_t opponent = 0; opponent < baselineCount; ++opponent)
			vulnerability += (dealtOnSubject[subject][opponent] + retaliatedByOpponent[subject][opponent]) / rawOutput[opponent];

		vulnerability = std::max(vulnerability / baselineCount, 1e-6);

		const double hitPoints = subjects[subject]->getBaseHitPoints()
			+ CombatValue::regeneratedHitPoints(*subjects[subject], CombatValue::referenceCount(subjects[subject]));

		Entry & entry = entries[subject];
		entry.effectiveHitPoints = hitPoints / vulnerability * survival[subject];
		entry.fightValue = CombatValue::combine(entry.output, entry.effectiveHitPoints, 1.0);
		entry.aiValue = entry.fightValue * entry.uptime;
	}

	// computed values have an arbitrary scale, so they are pinned to H3 values
	std::vector<double> fightScales;
	std::vector<double> aiScales;
	for(const auto & entry : entries)
	{
		if(!entry.baseline)
			continue;

		if(entry.fightValue > 0 && entry.creature->getFightValue() > 0)
			fightScales.push_back(entry.creature->getFightValue() / entry.fightValue);
		if(entry.aiValue > 0 && entry.creature->getAIValue() > 0)
			aiScales.push_back(entry.creature->getAIValue() / entry.aiValue);
	}

	if(fightScales.empty() || aiScales.empty())
		throw std::runtime_error("No creature of the original game declares a value to pin the scale to");

	const double scaleFight = CombatValue::median(fightScales);
	const double scaleAi = CombatValue::median(aiScales);

	for(auto & entry : entries)
	{
		entry.fightValue *= scaleFight;
		entry.aiValue *= scaleAi;
	}
}

void CreatureValueEstimator::applyFormula()
{
	for(auto & entry : entries)
	{
		// stack size that CombatValue itself assumes, so the two results are comparable
		const CStack * unit = placeStack(BattleSide::ATTACKER, entry.creature, BattleHex(attackerHex), CombatValue::referenceCount(entry.creature));

		// CombatValue returns an AI value, which is a fight value weighted by uptime
		entry.formulaAiValue = values.getAIValue(unit);
		entry.formulaFightValue = entry.formulaAiValue / CombatValue::uptimeOf(*unit);

		removeStack(unit);
	}

	// CombatValue pins only AI value to H3 scale; fight value is a diagnostic, pinned here instead
	std::vector<double> scales;
	for(const auto & entry : entries)
		if(entry.baseline && entry.formulaFightValue > 0 && entry.creature->getFightValue() > 0)
			scales.push_back(entry.creature->getFightValue() / entry.formulaFightValue);

	if(scales.empty())
		throw std::runtime_error("No creature of the original game declares a fight value to pin the scale to");

	const double scale = CombatValue::median(scales);

	for(auto & entry : entries)
		entry.formulaFightValue *= scale;
}

void CreatureValueEstimator::checkModelPaths()
{
	std::vector<double> ratios;
	int closer = 0;

	// heroes were stripped of their armies, so this is the only unit to approach
	const CStack * enemy = placeStack(BattleSide::DEFENDER, subjects.front(), BattleHex(defenderHex));

	for(const auto & entry : entries)
	{
		if(!entry.baseline)
			continue;

		// stack size that CombatValue itself assumes, so the two query paths are comparable
		const CStack * unit = placeStack(BattleSide::ATTACKER, entry.creature, BattleHex(attackerHex), CombatValue::referenceCount(entry.creature));

		const int64_t fromConfiguration = values.getAIValue(entry.creature);
		const int64_t asUnit = values.getAIValue(unit);
		const int64_t inBattle = values.getAIValue(unit, *battle());

		if(fromConfiguration > 0)
			ratios.push_back(static_cast<double>(asUnit) / fromConfiguration);

		// regeneration is the only effect whose per-creature value could depend on stack size
		if(entry.creature->hasBonusOfType(BonusType::HP_REGENERATION))
		{
			const CStack * few = placeStack(BattleSide::ATTACKER, entry.creature, BattleHex(attackerHex + 1), 5);
			const CStack * many = placeStack(BattleSide::ATTACKER, entry.creature, BattleHex(attackerHex + 2), 200);

			if(values.getAIValue(few) != values.getAIValue(many))
				logGlobal->error("%s has a different value per creature in a small stack than in a large one",
					entry.creature->getJsonKey());

			removeStack(few);
			removeStack(many);
		}

		// enemy stands closer than battle sides normally start, so approaching creatures gain here
		if(inBattle > asUnit)
			++closer;

		removeStack(unit);
	}

	removeStack(enemy);

	if(ratios.empty())
		throw std::runtime_error("No creature of the original game declares a value to check the model against");

	const auto range = std::ranges::minmax(ratios);

	logGlobal->info("Model paths: a creature answered for as a unit reaches %f of the value it has answered for out of its configuration (%f to %f); %d of %d gain once the enemy is in reach",
		CombatValue::median(ratios), range.min, range.max, closer, static_cast<int>(ratios.size()));

	// only creatures that cross the field in one turn are unaffected, and there are few of those
	if(closer == 0)
		logGlobal->error("No creature gained from the enemy being in reach - the battle has nothing to close on");
}

void CreatureValueEstimator::report() const
{
	logGlobal->info("Creature values measured at stack experience rank %d", experienceRank);
	logGlobal->info("creature,fightValue,derivedFightValue,formulaFightValue,aiValue,derivedAiValue,formulaAiValue");

	int agreed = 0;
	int differed = 0;
	int disagreed = 0;
	int unjudged = 0;

	// how far apart the two values are, whichever way round they sit
	const auto divergence = [](double derived, double declared)
	{
		if(derived <= 0 || declared <= 0)
			return std::numeric_limits<double>::infinity();

		return std::max(derived / declared, declared / derived);
	};

	for(const auto & entry : entries)
	{
		const int64_t derivedFight = std::llround(entry.fightValue);
		const int64_t derivedAi = std::llround(entry.aiValue);

		const std::string line = boost::str(boost::format("%s,%d,%d,%d,%d,%d,%d")
			% entry.creature->getJsonKey()
			% entry.creature->getFightValue()
			% derivedFight
			% std::llround(entry.formulaFightValue)
			% entry.creature->getAIValue()
			% derivedAi
			% std::llround(entry.formulaAiValue));

		// creature with no configured value can only be reported, not judged
		if(entry.creature->getFightValue() == 0 || entry.creature->getAIValue() == 0)
		{
			logGlobal->warn("%s", line);
			++unjudged;
			continue;
		}

		const double error = std::max(divergence(entry.fightValue, entry.creature->getFightValue()),
			divergence(entry.aiValue, entry.creature->getAIValue()));

		if(error <= agreementTolerance)
		{
			logGlobal->info("%s", line);
			++agreed;
		}
		else if(error <= disagreementTolerance)
		{
			logGlobal->warn("%s", line);
			++differed;
		}
		else
		{
			logGlobal->error("%s", line);
			++disagreed;
		}
	}

	logGlobal->info("%d creatures within a tenth of their declared value, %d further off, %d beyond half or double it, %d declaring no value to judge them by",
		agreed, differed, disagreed, unjudged);
}
