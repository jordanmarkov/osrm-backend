/*

Copyright (c) 2015, Project OSRM contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "extractor.hpp"

#include "extraction_containers.hpp"
#include "extraction_node.hpp"
#include "extraction_way.hpp"
#include "extractor_callbacks.hpp"
#include "restriction_parser.hpp"
#include "scripting_environment.hpp"

#include "../data_structures/raster_source.hpp"
#include "../util/make_unique.hpp"
#include "../util/simple_logger.hpp"
#include "../util/timing_util.hpp"
#include "../util/lua_util.hpp"

#include "../typedefs.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/optional/optional.hpp>

#include <luabind/luabind.hpp>

#include <osmium/io/any_input.hpp>

#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * TODO: Refactor this function into smaller functions for better readability.
 *
 * This function is the entry point for the whole extraction process. The goal of the extraction
 * step is to filter and convert the OSM geometry to something more fitting for routing.
 * That includes:
 *  - extracting turn restrictions
 *  - splitting ways into (directional!) edge segments
 *  - checking if nodes are barriers or traffic signal
 *  - discarding all tag information: All relevant type information for nodes/ways
 *    is extracted at this point.
 *
 * The result of this process are the following files:
 *  .names : Names of all streets, stored as long consecutive string with prefix sum based index
 *  .osrm  : Nodes and edges in a intermediate format that easy to digest for osrm-prepare
 *  .restrictions : Turn restrictions that are used my osrm-prepare to construct the edge-expanded graph
 *
 */
int extractor::run()
{
    try
    {
        LogPolicy::GetInstance().Unmute();
        TIMER_START(extracting);

        const unsigned recommended_num_threads = tbb::task_scheduler_init::default_num_threads();
        const auto number_of_threads =
            std::min(recommended_num_threads, config.requested_num_threads);
        tbb::task_scheduler_init init(number_of_threads);

        SimpleLogger().Write() << "Input file: " << config.input_path.filename().string();
        SimpleLogger().Write() << "Profile: " << config.profile_path.filename().string();
        SimpleLogger().Write() << "Threads: " << number_of_threads;

        // setup scripting environment
        ScriptingEnvironment scripting_environment(config.profile_path.string().c_str());

        ExtractionContainers extraction_containers;
        auto extractor_callbacks = osrm::make_unique<ExtractorCallbacks>(extraction_containers);

        const osmium::io::File input_file(config.input_path.string());
        osmium::io::Reader reader(input_file);
        const osmium::io::Header header = reader.header();

        std::atomic<unsigned> number_of_nodes{0};
        std::atomic<unsigned> number_of_ways{0};
        std::atomic<unsigned> number_of_relations{0};
        std::atomic<unsigned> number_of_others{0};

        SimpleLogger().Write() << "Parsing in progress..";
        TIMER_START(parsing);

        lua_State *segment_state = scripting_environment.get_lua_state();

        if (lua_function_exists(segment_state, "source_function"))
        {
            // bind a single instance of SourceContainer class to relevant lua state
            SourceContainer sources;
            luabind::globals(segment_state)["sources"] = sources;

            luabind::call_function<void>(segment_state, "source_function");
        }

        std::string generator = header.get("generator");
        if (generator.empty())
        {
            generator = "unknown tool";
        }
        SimpleLogger().Write() << "input file generated by " << generator;

        // write .timestamp data file
        std::string timestamp = header.get("osmosis_replication_timestamp");
        if (timestamp.empty())
        {
            timestamp = "n/a";
        }
        SimpleLogger().Write() << "timestamp: " << timestamp;

        boost::filesystem::ofstream timestamp_out(config.timestamp_file_name);
        timestamp_out.write(timestamp.c_str(), timestamp.length());
        timestamp_out.close();

        // initialize vectors holding parsed objects
        tbb::concurrent_vector<std::pair<std::size_t, ExtractionNode>> resulting_nodes;
        tbb::concurrent_vector<std::pair<std::size_t, ExtractionWay>> resulting_ways;
        tbb::concurrent_vector<boost::optional<InputRestrictionContainer>> resulting_restrictions;

        // setup restriction parser
        const RestrictionParser restriction_parser(scripting_environment.get_lua_state());

        while (const osmium::memory::Buffer buffer = reader.read())
        {
            // create a vector of iterators into the buffer
            std::vector<osmium::memory::Buffer::const_iterator> osm_elements;
            for (auto iter = std::begin(buffer), end = std::end(buffer); iter != end; ++iter)
            {
                osm_elements.push_back(iter);
            }

            // clear resulting vectors
            resulting_nodes.clear();
            resulting_ways.clear();
            resulting_restrictions.clear();

            // parse OSM entities in parallel, store in resulting vectors
            tbb::parallel_for(
                tbb::blocked_range<std::size_t>(0, osm_elements.size()),
                [&](const tbb::blocked_range<std::size_t> &range)
                {
                    ExtractionNode result_node;
                    ExtractionWay result_way;
                    lua_State *local_state = scripting_environment.get_lua_state();

                    for (auto x = range.begin(), end = range.end(); x != end; ++x)
                    {
                        const auto entity = osm_elements[x];

                        switch (entity->type())
                        {
                        case osmium::item_type::node:
                            result_node.clear();
                            ++number_of_nodes;
                            luabind::call_function<void>(
                                local_state, "node_function",
                                boost::cref(static_cast<const osmium::Node &>(*entity)),
                                boost::ref(result_node));
                            resulting_nodes.push_back(std::make_pair(x, result_node));
                            break;
                        case osmium::item_type::way:
                            result_way.clear();
                            ++number_of_ways;
                            luabind::call_function<void>(
                                local_state, "way_function",
                                boost::cref(static_cast<const osmium::Way &>(*entity)),
                                boost::ref(result_way));
                            resulting_ways.push_back(std::make_pair(x, result_way));
                            break;
                        case osmium::item_type::relation:
                            ++number_of_relations;
                            resulting_restrictions.push_back(restriction_parser.TryParse(
                                static_cast<const osmium::Relation &>(*entity)));
                            break;
                        default:
                            ++number_of_others;
                            break;
                        }
                    }
                });

            // put parsed objects thru extractor callbacks
            for (const auto &result : resulting_nodes)
            {
                extractor_callbacks->ProcessNode(
                    static_cast<const osmium::Node &>(*(osm_elements[result.first])),
                    result.second);
            }
            for (const auto &result : resulting_ways)
            {
                extractor_callbacks->ProcessWay(
                    static_cast<const osmium::Way &>(*(osm_elements[result.first])), result.second);
            }
            for (const auto &result : resulting_restrictions)
            {
                extractor_callbacks->ProcessRestriction(result);
            }
        }
        TIMER_STOP(parsing);
        SimpleLogger().Write() << "Parsing finished after " << TIMER_SEC(parsing) << " seconds";

        SimpleLogger().Write() << "Raw input contains " << number_of_nodes.load() << " nodes, "
                               << number_of_ways.load() << " ways, and "
                               << number_of_relations.load() << " relations, and "
                               << number_of_others.load() << " unknown entities";

        extractor_callbacks.reset();

        if (extraction_containers.all_edges_list.empty())
        {
            SimpleLogger().Write(logWARNING) << "The input data is empty, exiting.";
            return 1;
        }

        extraction_containers.PrepareData(config.output_file_name,
                                          config.restriction_file_name,
                                          config.names_file_name,
                                          segment_state);

        TIMER_STOP(extracting);
        SimpleLogger().Write() << "extraction finished after " << TIMER_SEC(extracting) << "s";
        SimpleLogger().Write() << "To prepare the data for routing, run: "
                               << "./osrm-prepare " << config.output_file_name
                               << std::endl;
    }
    catch (std::exception &e)
    {
        SimpleLogger().Write(logWARNING) << e.what();
        return 1;
    }
    return 0;
}
