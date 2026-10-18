/*
 * delegation.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */

#pragma once

namespace MMAI::BAI
{

// Unit types which MMAI models can not control well and which can be handed over to BattleAI instead.
// Configured in mmai-settings.json ("delegateToBattleAI"), everything is off by default.
struct BattleAIDelegation
{
	// Units with an active spellcasting ability ask BattleAI first. If BattleAI decides
	// to cast a spell, that spell is cast; otherwise the model controls the unit as usual.
	bool creatureSpells = false;
};

}
