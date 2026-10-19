/*
 * EntryPoint.cpp, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#include "StdInc.h"

#include <vstd/DateUtils.h>

#include "../server/CVCMIServer.h"
#include "../server/CreatureValueEstimator.h"
#include "../server/CreatureDuels.h"

#include "../lib/CConsoleHandler.h"
#include "../lib/logging/CBasicLogConfigurator.h"
#include "../lib/logging/CLogger.h"
#include "../lib/VCMIDirs.h"
#include "../lib/GameLibrary.h"
#include "../lib/CConfigHandler.h"
#include "../lib/filesystem/Filesystem.h"
#include "../lib/modding/CModHandler.h"
#include "../lib/modding/ModManager.h"
#include "callback/EditorCallback.h"
#include "campaign/CampaignHandler.h"
#include "mapping/CMap.h"
#include "mapping/CMapService.h"
#include "modding/ModDescription.h"
#include "texts/CGeneralTextHandler.h"
#include "texts/TextOperations.h"
#include "../luascript/LuaModule.h"

#include <boost/program_options.hpp>

#include <vcmi/scripting/Service.h>

static const std::string SERVER_NAME_AFFIX = "server";
static const std::string SERVER_NAME = std::string(GameConstants::VCMI_PROJECT_NAME_VERSIONED) + " (" + SERVER_NAME_AFFIX + ')';

static void exportLuaApiDocs(const boost::filesystem::path & outPath)
{
	auto scriptHandler = std::make_unique<scripting::LuaModule>();
	scriptHandler->exportDocs(outPath);

	logGlobal->info("Lua API documentation export complete");
	logGlobal->info("Generated files can be found in " + TextOperations::filesystemPathToUtf8(outPath) + " directory");
}

static void generateTranslations(const std::string & modID)
{
	LIBRARY = new GameLibrary;
	LIBRARY->loadFilesystem(false);
	settings.init("config/settings.json", "vcmi:settings");

	auto mods = std::make_unique<ModManager>();

	std::string oldPresetName = mods->getActivePreset();
	mods->createNewPreset("translation-export");
	mods->activatePreset("translation-export");
	mods = std::make_unique<ModManager>();
	mods->tryEnableMods({modID});

	for (const auto & submod : mods->getModSettings(modID))
	{
		try
		{
			if (!submod.second)
				mods->tryEnableMods({modID + '.' + submod.first});
		}
		catch (const std::exception &)
		{
			// failed to enable mod - ignore, will be logged later
		}
	}

	for (const auto & submod : mods->getModSettings(modID))
		if (!submod.second)
			logGlobal->warn("Failed to enable submod %s", submod.first);

	std::map<std::string, ExportedStrings> textsByMod;
	std::vector<std::string> modsWithOverrides;

	delete LIBRARY;
	LIBRARY = new GameLibrary;
	LIBRARY->initializeFilesystem(false);
	LIBRARY->initializeLibrary();

	{
		CMapService mapService;

		logGlobal->info("Searching for available maps");
		std::unordered_set<ResourcePath> mapList = CResourceHandler::get()->getFilteredFiles([&](const ResourcePath & ident)
		{
			return ident.getType() == EResType::MAP;
		});

		std::vector<std::unique_ptr<CMap>> loadedMaps;
		std::vector<std::shared_ptr<CampaignState>> loadedCampaigns;

		logGlobal->info("Loading maps for export");
		for (auto const & mapName : mapList)
		{
			try
			{
				std::string mapModName = LIBRARY->modh->findResourceOrigin(mapName);
				if (mapModName != modID && !mapModName.starts_with(modID + '.'))
					continue;

				EditorCallback cb(nullptr);
				// load and drop loaded map - we only need loader to run over all maps
				loadedMaps.push_back(mapService.loadMap(mapName, &cb));
			}
			catch(std::exception & e)
			{
				logGlobal->warn("Map %s is invalid. Message: %s", mapName.getName(), e.what());
			}
		}

		logGlobal->info("Searching for available campaigns");
		std::unordered_set<ResourcePath> campaignList = CResourceHandler::get()->getFilteredFiles([&](const ResourcePath & ident)
		{
			return ident.getType() == EResType::CAMPAIGN;
		});

		logGlobal->info("Loading campaigns for export");
		for (auto const & campaignName : campaignList)
		{
			try
			{
				std::string campaignModName = LIBRARY->modh->findResourceOrigin(campaignName);
				if (campaignModName != modID && !campaignModName.starts_with(modID + '.'))
					continue;

				loadedCampaigns.push_back(CampaignHandler::getCampaign(campaignName.getName()));
				for (auto const & part : loadedCampaigns.back()->allScenarios())
				{
					EditorCallback cb(nullptr);
					loadedCampaigns.back()->getMap(part, &cb);
				}
			}
			catch(std::exception & e)
			{
				logGlobal->warn("Campaign %s is invalid. Message: %s", campaignName.getName(), e.what());
			}
		}

		LIBRARY->generaltexth->exportAllTexts(textsByMod, false);

		// map and campaign texts are owned by maps and campaigns themselves, not by the library
		for (auto const & map : loadedMaps)
			map->texts->exportAllTexts(textsByMod, false);

		for (auto const & campaign : loadedCampaigns)
		{
			campaign->getTexts()->exportAllTexts(textsByMod, false);
			for (auto const & scenarioTexts : campaign->getScenarioTexts())
				scenarioTexts.second->exportAllTexts(textsByMod, false);
		}
	}

	for(const auto & modEntry : textsByMod)
	{
		if (modEntry.first.find('.') != std::string::npos)
		{
			for (const auto & otherModID : modEntry.second.overridenMods)
			{
				if (otherModID == modID || otherModID.starts_with(modID + '.'))
				{
					modsWithOverrides.push_back(modEntry.first);
					break;
				}
			}
		}
	}

	const boost::filesystem::path outPath = VCMIDirs::get().userExtractedPath() / "translationFull";
	boost::filesystem::create_directories(outPath);

	mods->createNewPreset("translation-export-base");
	mods->activatePreset("translation-export-base");
	mods = std::make_unique<ModManager>();
	mods->tryEnableMods({modID});

	for (const auto & submod : mods->getModSettings(modID))
	{
		try
		{
			std::string fullModID = modID + '.' + submod.first;
			bool hasOverrides = vstd::contains(modsWithOverrides, fullModID);
			if (!submod.second && !hasOverrides)
				mods->tryEnableMods({fullModID});

			if (submod.second && hasOverrides)
				mods->tryDisableMod(fullModID);
		}
		catch (const std::exception &)
		{
			// failed to enable mod - ignore, will be logged later
		}
	}


	CResourceHandler::destroy();
	delete LIBRARY;
	LIBRARY = new GameLibrary;
	LIBRARY->initializeFilesystem(false);
	LIBRARY->initializeLibrary();
	LIBRARY->generaltexth->exportAllTexts(textsByMod, false);

	for(const auto & modEntry : textsByMod)
	{
		JsonNode output;

		if (modEntry.first != modID && !modEntry.first.starts_with(modID + '.'))
			continue;

		for(const auto & stringEntry : modEntry.second.strings)
			output[stringEntry.first].String() = stringEntry.second;

		if (!output.isNull())
		{
			std::string preferredLanguage = LIBRARY->generaltexth->getPreferredLanguage();
			std::string filename = boost::replace_all_copy(modEntry.first, ".", "/Mods/");
			const boost::filesystem::path dirPath = outPath / filename / "Content/translation/";
			boost::filesystem::create_directories(dirPath);
			const boost::filesystem::path filePath = dirPath / (preferredLanguage + ".json");
			std::ofstream file(filePath.c_str());
			file << output.toString();
		}
	}

	mods->activatePreset(oldPresetName);
	mods->deletePreset("translation-export");
	mods->deletePreset("translation-export-base");
	logGlobal->info("Translation export complete");
	logGlobal->info("Extracted files can be found in " + TextOperations::filesystemPathToUtf8(outPath) + " directory\n");

}

static void handleCommandOptions(int argc, const char * argv[], boost::program_options::variables_map & options)
{
	boost::program_options::options_description opts("Allowed options");
	opts.add_options()
	("help,h", "display help and exit")
	("version,v", "display version information and exit")
	("run-by-client", "indicate that server launched by client on same machine")
	("dummy-run", "Shutdown immediately after loading was sucessful")
	("creature-values", "Compute AI values and fight values of all creatures and compare them against configured values")
	("creature-values-rank", boost::program_options::value<int>(), "Same as creature-values, with every measured stack at given stack experience rank")
	("creature-duels", boost::program_options::value<std::string>(), "Measure all creatures in complete battles against creatures of the original game, with battle AI on both sides, appending results to given CSV file")
	("creature-duels-rank", boost::program_options::value<int>(), "Stack experience rank of all units in creature duels, 0 by default")
	("creature-duels-shard", boost::program_options::value<std::string>(), "Part of creatures to measure in this process, as index/count with index starting at 1, e.g. 2/4")
	("creature-duels-references", boost::program_options::value<int>(), "Number of original game creatures each creature is measured against, 4 by default")
	("creature-duels-ai", boost::program_options::value<std::string>(), "Battle AI controlling both sides of creature duels, BattleAI by default")
	("creature-duels-subjects", boost::program_options::value<std::string>(), "File with identifiers of creatures to measure in creature duels, one per line, all creatures by default")
	("translate-mod", boost::program_options::value<std::string>(), "Export translations for specified mod")
	("export-lua-docs", boost::program_options::value<std::string>(), "Export Lua scripting API documentation to specified directory")
	("port", boost::program_options::value<ui16>(), "port at which server will listen to connections from client")
	("lobby", "start server in lobby mode in which server connects to a global lobby");

	if(argc > 1)
	{
		try
		{
			boost::program_options::store(boost::program_options::parse_command_line(argc, argv, opts), options);
		}
		catch(boost::program_options::error & e)
		{
			std::cerr << "Failure during parsing command-line options:\n" << e.what() << std::endl;
		}
	}

	boost::program_options::notify(options);

	if(options.count("help"))
	{
		auto time = std::time(nullptr);
		std::tm tm = vstd::safeLocalTime(time);
		printf("%s - A Heroes of Might and Magic 3 clone\n", GameConstants::VCMI_PROJECT_NAME_VERSIONED);
		printf("Copyright (C) 2007-%d VCMI dev team - see AUTHORS file\n", tm.tm_year + 1900);
		printf("This is free software; see the source for copying conditions. There is NO\n");
		printf("warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
		printf("\n");
		std::cout << opts;
		exit(0);
	}

	if(options.count("translate-mod"))
	{
		std::string modID = options["translate-mod"].as<std::string>();
		generateTranslations(modID);
		exit(0);
	}

	if(options.count("export-lua-docs"))
	{
		exportLuaApiDocs(options["export-lua-docs"].as<std::string>());
		exit(0);
	}

	if(options.count("version"))
	{
		printf("%s\n", GameConstants::VCMI_VERSION);
		std::cout << VCMIDirs::get().genHelpString();
		exit(0);
	}
}

static std::pair<int, int> parseShard(const std::string & text)
{
	std::vector<std::string> parts;
	boost::split(parts, text, boost::is_any_of("/"));
	if(parts.size() != 2)
		throw std::runtime_error("Creature duels shard must be given as index/count, e.g. 2/4");

	const int index = std::stoi(parts[0]);
	const int count = std::stoi(parts[1]);
	if(count < 1 || index < 1 || index > count)
		throw std::runtime_error("Creature duels shard index must be between 1 and count");

	return {index - 1, count};
}

/// Several processes measuring creature duels at once would otherwise all write to the same log
static std::string serverLogName(int argc, const char * argv[])
{
	bool duels = false;
	std::string shard;
	for(int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if(argument == "--creature-duels" || argument.starts_with("--creature-duels="))
			duels = true;
		if(argument == "--creature-duels-shard" && index + 1 < argc)
			shard = argv[index + 1];
		if(argument.starts_with("--creature-duels-shard="))
			shard = argument.substr(std::string("--creature-duels-shard=").size());
	}

	if(!duels)
		return "VCMI_Server_log.txt";

	boost::replace_all(shard, "/", "-");
	shard.erase(std::remove_if(shard.begin(), shard.end(), [](char symbol){ return !std::isdigit(static_cast<unsigned char>(symbol)) && symbol != '-'; }), shard.end());
	return shard.empty() ? "VCMI_Server_duels_log.txt" : "VCMI_Server_duels_" + shard + "_log.txt";
}

static void runCreatureDuels(const boost::program_options::variables_map & opts)
{
	// every battle AI decision is logged at trace level, which would slow duels down many times
	CLogger::getGlobalLogger()->setLevel(ELogLevel::INFO);
	for(const auto & loggerNode : settings["logging"]["loggers"].Vector())
	{
		CLogger * logger = CLogger::getLogger(CLoggerDomain(loggerNode["domain"].String()));
		if(logger->getLevel() < ELogLevel::INFO)
			logger->setLevel(ELogLevel::INFO);
	}

	CreatureDuels::Options options;
	options.output = opts["creature-duels"].as<std::string>();
	if(opts.count("creature-duels-rank"))
		options.experienceRank = opts["creature-duels-rank"].as<int>();
	if(opts.count("creature-duels-shard"))
		std::tie(options.shardIndex, options.shardCount) = parseShard(opts["creature-duels-shard"].as<std::string>());
	if(opts.count("creature-duels-references"))
		options.references = std::max(1, opts["creature-duels-references"].as<int>());
	if(opts.count("creature-duels-ai"))
		options.battleAI = opts["creature-duels-ai"].as<std::string>();
	if(opts.count("creature-duels-subjects"))
		options.subjects = opts["creature-duels-subjects"].as<std::string>();

	CreatureDuels::run(options);
}

int main(int argc, const char * argv[])
{
	// Correct working dir executable folder (not bundle folder) so we can use executable relative paths
	boost::filesystem::current_path(boost::filesystem::system_complete(argv[0]).parent_path());

	CConsoleHandler console;
	CBasicLogConfigurator logConfigurator(VCMIDirs::get().userLogsPath() / serverLogName(argc, argv), &console);
	logConfigurator.configureDefault();
	logGlobal->info(SERVER_NAME);

	boost::program_options::variables_map opts;
	handleCommandOptions(argc, argv, opts);
	LIBRARY = new GameLibrary;
	LIBRARY->initializeFilesystem(false);
	logConfigurator.configure();

	LIBRARY->initializeLibrary();

	if(opts.count("creature-duels"))
	{
		runCreatureDuels(opts);
	}
	else if(opts.count("creature-values") || opts.count("creature-values-rank"))
	{
		CreatureValueEstimator::run(opts.count("creature-values-rank") ? opts["creature-values-rank"].as<int>() : 0);
	}
	else if(!opts.count("dummy-run"))
	{
		bool connectToLobby = opts.count("lobby");
		bool runByClient = opts.count("runByClient");
		uint16_t port = settings["server"]["localPort"].Integer();
		if(opts.count("port"))
			port = opts["port"].as<uint16_t>();

		CVCMIServer server(port, runByClient);
		server.prepare(connectToLobby, true);
		server.run();
		// CVCMIServer destructor must be called here - before LIBRARY cleanup
	}

	logConfigurator.deconfigure();

	delete LIBRARY;
	return 0;
}
