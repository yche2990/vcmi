/*
 * BattlegroundGame.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include "../lib/battle/BattleSide.h"
#include "../lib/constants/EntityIdentifiers.h"

class CGameState;
class CGHeroInstance;
class IMapService;

/// The smallest game a battle can happen in: bare sand and one hero per player, both heroes
/// stripped of army, skills and specialty. Used by tools that measure creatures in battles
class BattlegroundGame
{
public:
	BattlegroundGame();
	~BattlegroundGame();

	std::shared_ptr<CGameState> gameState;
	/// Hero of player 0 on the attacker side and of player 1 on the defender side
	BattleSideArray<CGHeroInstance *> heroes = {};

	/// Battlefield without luck, morale or other bonuses of its own
	static BattleField neutralBattlefield();

private:
	class MapService;

	std::unique_ptr<IMapService> mapService;

	/// Strips everything from a hero that could modify battle results
	static void stripHeroBonuses(CGHeroInstance * hero);
};
