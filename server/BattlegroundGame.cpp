/*
 * BattlegroundGame.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include "BattlegroundGame.h"

#include "../lib/CRandomGenerator.h"
#include "../lib/CSkillHandler.h"
#include "../lib/GameLibrary.h"
#include "../lib/LoadProgress.h"
#include "../lib/StartInfo.h"
#include "../lib/callback/GameRandomizer.h"
#include "../lib/entities/hero/CHero.h"
#include "../lib/entities/hero/CHeroClass.h"
#include "../lib/gameState/CGameState.h"
#include "../lib/mapObjectConstructors/AObjectTypeHandler.h"
#include "../lib/mapObjectConstructors/CObjectClassesHandler.h"
#include "../lib/mapObjects/CGHeroInstance.h"
#include "../lib/mapping/CMap.h"
#include "../lib/mapping/CMapEditManager.h"
#include "../lib/mapping/CMapService.h"
#include "../lib/mapping/MapEditUtils.h"
#include "../lib/mapping/MapFormat.h"
#include "../lib/modding/IdentifierStorage.h"
#include "../lib/modding/ModScope.h"

/// Built here instead of generated, since a random map takes minutes and depends on templates of loaded mods
class BattlegroundGame::MapService final : public IMapService
{
public:
	std::unique_ptr<CMap> loadMap(const ResourcePath &, IGameInfoCallback * cb) const override
	{
		auto map = std::make_unique<CMap>(cb);

		map->version = EMapFormat::VCMI;
		map->creationDateTime = std::time(nullptr);
		map->width = mapSize;
		map->height = mapSize;
		map->mapLayers = {MapLayerId::SURFACE};
		map->name.appendRawString("Battleground");
		map->initTerrain();
		// sand is native to no faction, so nobody gains the native terrain bonus
		map->getEditManager()->getTerrainSelection().selectAll();
		map->getEditManager()->drawTerrain(ETerrainId::SAND, 0, &CRandomGenerator::getDefault());

		for(int index = 0; index < 2; ++index)
		{
			PlayerInfo & player = map->players.at(index);
			player.canHumanPlay = true;
			player.canComputerPlay = true;
			player.isFactionRandom = true;

			placeHero(*map, cb, PlayerColor(index), HeroTypeID(index), int3(2 + index * 4, 2, 0));
		}

		return map;
	}

	std::unique_ptr<CMapHeader> loadMapHeader(const ResourcePath &, bool = false) const override { return nullptr; }
	std::unique_ptr<CMap> loadMap(const ui8 *, int, const std::string &, const std::string &, const std::string &, IGameInfoCallback *) const override { return nullptr; }
	std::unique_ptr<CMapHeader> loadMapHeader(const ui8 *, int, const std::string &, const std::string &, const std::string &) const override { return nullptr; }
	void saveMap(const std::unique_ptr<CMap> &, boost::filesystem::path) const override {}

private:
	static constexpr int mapSize = 12;

	static void placeHero(CMap & map, IGameInfoCallback * cb, const PlayerColor & owner, const HeroTypeID & type, const int3 & position)
	{
		auto handler = LIBRARY->objtypeh->getHandlerFor(Obj::HERO, type.toHeroType()->heroClass->getIndex());
		auto hero = std::dynamic_pointer_cast<CGHeroInstance>(handler->create(cb, handler->getTemplates().front()));

		hero->ID = Obj::HERO;
		hero->setHeroType(type);
		hero->tempOwner = owner;
		hero->setAnchorPos(position + hero->getVisitableOffset());

		map.getEditManager()->insertObject(hero);
	}
};

BattlegroundGame::BattlegroundGame()
{
	logGlobal->info("Preparing a game to measure creatures in");

	StartInfo startInfo;
	startInfo.mode = EStartMode::NEW_GAME;
	startInfo.difficulty = static_cast<ui8>(EMapDifficulty::EASY);
	startInfo.mapname = "battleground";

	for(int index = 0; index < 2; ++index)
	{
		PlayerSettings & settings = startInfo.playerInfos[PlayerColor(index)];
		settings.color = PlayerColor(index);
		settings.name = "Player";
		settings.bonus = PlayerStartingBonus::GOLD; // no random starting artifact
	}

	mapService = std::make_unique<MapService>();

	gameState = std::make_shared<CGameState>();
	gameState->preInit(LIBRARY);

	GameRandomizer randomizer(*gameState);
	Load::ProgressAccumulator progressTracker;
	gameState->init(mapService.get(), &startInfo, randomizer, progressTracker, false);

	for(auto * hero : gameState->getMap().getObjects<CGHeroInstance>())
	{
		if(hero->getOwner() == PlayerColor(0) && heroes[BattleSide::ATTACKER] == nullptr)
			heroes[BattleSide::ATTACKER] = hero;
		if(hero->getOwner() == PlayerColor(1) && heroes[BattleSide::DEFENDER] == nullptr)
			heroes[BattleSide::DEFENDER] = hero;
	}

	if(heroes[BattleSide::ATTACKER] == nullptr || heroes[BattleSide::DEFENDER] == nullptr)
		throw std::runtime_error("Battleground map has no hero for one of its two players");

	stripHeroBonuses(heroes[BattleSide::ATTACKER]);
	stripHeroBonuses(heroes[BattleSide::DEFENDER]);
}

BattlegroundGame::~BattlegroundGame() = default;

BattleField BattlegroundGame::neutralBattlefield()
{
	// default battlefield is a clover field, whose luck bonus would skew battle results
	return BattleField(LIBRARY->identifiers()->getIdentifier(ModScope::scopeBuiltin(), "battlefield.sand_shore").value());
}

void BattlegroundGame::stripHeroBonuses(CGHeroInstance * hero)
{
	// starting army would join the battle next to the measured units, carrying battle-wide bonuses
	hero->clearSlots();

	for(const auto & bonus : hero->getHeroType()->specialty)
		hero->removeBonus(bonus);

	for(size_t index = 0; index < LIBRARY->skillh->size(); ++index)
		hero->setSecSkillLevel(SecondarySkill(static_cast<int32_t>(index)), 0, ChangeValueMode::ABSOLUTE);

	for(auto skill : {PrimarySkill::ATTACK, PrimarySkill::DEFENSE, PrimarySkill::SPELL_POWER, PrimarySkill::KNOWLEDGE})
		hero->setPrimarySkill(skill, 0, ChangeValueMode::ABSOLUTE);
}
