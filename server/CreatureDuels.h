/*
 * CreatureDuels.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

/// Measures every loaded creature in complete battles against creatures of the original game,
/// with a battle AI controlling both sides, so that spells, combat scripts and every other ability
/// take part. For each pair it searches the number of opponents that the measured stack is even
/// against, and appends one line per pair to a CSV file. Diagnostic tool, run from command line
class CreatureDuels
{
public:
	struct Options
	{
		/// CSV file results are appended to; pairs already present in it are not measured again
		boost::filesystem::path output;
		/// Stack experience rank of every unit in every battle
		int experienceRank = 0;
		/// Part of all measured creatures that this process measures, zero-based, out of shardCount
		int shardIndex = 0;
		int shardCount = 1;
		/// Number of creatures of the original game each creature is measured against
		int references = 4;
		/// Battle AI controlling both sides
		std::string battleAI = "BattleAI";
		/// Optional file with creature identifiers, one per line, to measure only those
		boost::filesystem::path subjects;
		/// When set, measures hero builds of factions instead of creatures: each build leads its
		/// faction's army against the same army led by a stripped hero, and the scale that army is
		/// even at is written for each build. File with the factions, their armies and builds, as JSON
		boost::filesystem::path factionBuilds;
		/// When set, measures faction armies against each other instead: each side is the army a
		/// faction raised by some day, led by a real hero of the faction with its specialty, spells,
		/// commander and the bonuses of its town buildings. For each pair of sides the second side's
		/// army is scaled until the two are even. File with the sides and pairs, as JSON
		boost::filesystem::path armyJobs;
	};

	static void run(const Options & options);
};
