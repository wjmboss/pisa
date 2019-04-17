#include <iostream>

#include <functional>
#include "boost/algorithm/string/classification.hpp"
#include "boost/algorithm/string/split.hpp"
#include "boost/optional.hpp"

#include "accumulator/lazy_accumulator.hpp"
#include "mio/mmap.hpp"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include "mappable/mapper.hpp"

#include "cursor/block_max_scored_cursor.hpp"
#include "cursor/max_scored_cursor.hpp"
#include "cursor/scored_cursor.hpp"
#include "index_types.hpp"
#include "io.hpp"
#include "query/queries.hpp"
#include "util/util.hpp"
#include "wand_data_compressed.hpp"
#include "wand_data_raw.hpp"

#include "CLI/CLI.hpp"

using namespace pisa;
using ranges::view::enumerate;

template <typename IndexType, typename WandType>
void evaluate_queries(const std::string &                 index_filename,
                      const boost::optional<std::string> &wand_data_filename,
                      const std::vector<Query> &          queries,
                      const boost::optional<std::string> &thresholds_filename,
                      std::string const &type, std::string const &query_type, uint64_t k,
                      std::string const &documents_filename, std::string const &iteration = "Q0",
                      std::string const &run_id = "R0") {
    IndexType        index;
    mio::mmap_source m(index_filename.c_str());
    mapper::map(index, m);

    WandType wdata;

    mio::mmap_source md;
    if (wand_data_filename) {
        std::error_code error;
        md.map(*wand_data_filename, error);
        if (error) {
            spdlog::error("error mapping file: {}, exiting...", error.message());
            std::abort();
        }
        mapper::map(wdata, md, mapper::map_flags::warmup);
    }

    auto docmap = io::read_string_vector(documents_filename);

    std::function<std::vector<std::pair<float, uint64_t>>(term_id_vec)> query_fun;

    if (query_type == "wand" && wand_data_filename) {
        query_fun = [&](term_id_vec terms) {
            wand_query wand_q(k);
            wand_q(make_max_scored_cursors(index, wdata, terms), index.num_docs());
            return wand_q.topk();
        };
    } else if (query_type == "block_max_wand" && wand_data_filename) {
        query_fun = [&](term_id_vec terms) {
            block_max_wand_query block_max_wand_q(k);
            block_max_wand_q(make_block_max_scored_cursors(index, wdata, terms), index.num_docs());
            return block_max_wand_q.topk();
        };
    } else if (query_type == "block_max_maxscore" && wand_data_filename) {
        query_fun = [&](term_id_vec terms) {
            block_max_maxscore_query block_max_maxscore_q(k);
            block_max_maxscore_q(make_block_max_scored_cursors(index, wdata, terms),
                                 index.num_docs());
            return block_max_maxscore_q.topk();
        };
    } else if (query_type == "ranked_and" && wand_data_filename) {
        query_fun = [&](term_id_vec terms) {
            ranked_and_query ranked_and_q(k);
            ranked_and_q(make_scored_cursors(index, wdata, terms), index.num_docs());
            return ranked_and_q.topk();
        };
    } else if (query_type == "ranked_or" && wand_data_filename) {
        query_fun = [&](term_id_vec terms) {
            ranked_or_query ranked_or_q(k);
            ranked_or_q(make_scored_cursors(index, wdata, terms), index.num_docs());
            return ranked_or_q.topk();
        };
    } else if (query_type == "maxscore" && wand_data_filename) {
        query_fun = [&](term_id_vec terms) {
            maxscore_query maxscore_q(k);
            maxscore_q(make_max_scored_cursors(index, wdata, terms), index.num_docs());
            return maxscore_q.topk();
        };
    } else if (query_type == "ranked_or_taat" && wand_data_filename) {
        Simple_Accumulator   accumulator(index.num_docs());
        ranked_or_taat_query ranked_or_taat_q(k);
        query_fun = [&, ranked_or_taat_q](term_id_vec terms) mutable {
            ranked_or_taat_q(make_scored_cursors(index, wdata, terms), index.num_docs(),
                             accumulator);
            return ranked_or_taat_q.topk();
        };
    } else if (query_type == "ranked_or_taat_lazy" && wand_data_filename) {
        Lazy_Accumulator<4>  accumulator(index.num_docs());
        ranked_or_taat_query ranked_or_taat_q(k);
        query_fun = [&, ranked_or_taat_q](term_id_vec terms) mutable {
            ranked_or_taat_q(make_scored_cursors(index, wdata, terms), index.num_docs(),
                             accumulator);
            return ranked_or_taat_q.topk();
        };
    } else {
        spdlog::error("Unsupported query type: {}", query_type);
    }

    for (auto const & [ qid, query ] : enumerate(queries)) {
        auto results = query_fun(query.terms);
        for (auto && [ rank, result ] : enumerate(results)) {
            std::cout << fmt::format("{}\t{}\t{}\t{}\t{}\t{}\n",
                                     query.id.value_or(std::to_string(qid)), iteration,
                                     docmap.at(result.second), rank, result.first, run_id);
        }
    }
}

using wand_raw_index     = wand_data<bm25, wand_data_raw<bm25>>;
using wand_uniform_index = wand_data<bm25, wand_data_compressed<bm25, uniform_score_compressor>>;

int main(int argc, const char **argv) {
    spdlog::set_default_logger(spdlog::stderr_color_mt("default"));

    std::string                  type;
    std::string                  query_type;
    std::string                  index_filename;
    std::optional<std::string>   terms_file;
    std::string                  documents_file;
    boost::optional<std::string> wand_data_filename;
    boost::optional<std::string> query_filename;
    boost::optional<std::string> thresholds_filename;
    std::optional<std::string>   stopwords_filename;
    std::optional<std::string>   stemmer    = std::nullopt;
    uint64_t                     k          = configuration::get().k;
    bool                         compressed = false;

    CLI::App app{"queries - a tool for performing queries on an index."};
    app.set_config("--config", "", "Configuration .ini file", false);
    app.add_option("-t,--type", type, "Index type")->required();
    app.add_option("-a,--algorithm", query_type, "Query algorithm")->required();
    app.add_option("-i,--index", index_filename, "Collection basename")->required();
    app.add_option("-w,--wand", wand_data_filename, "Wand data filename");
    app.add_option("-q,--query", query_filename, "Queries filename");
    app.add_flag("--compressed-wand", compressed, "Compressed wand input file");
    app.add_option("--stopwords", stopwords_filename, "File containing stopwords to ignore");
    app.add_option("-k", k, "k value");
    auto *terms_opt =
        app.add_option("--terms", terms_file, "Text file with terms in separate lines");
    app.add_option("--stemmer", stemmer, "Stemmer type")->needs(terms_opt);
    app.add_option("--documents", documents_file, "Text file with documents in separate lines")
        ->required();
    CLI11_PARSE(app, argc, argv);

    std::vector<Query> queries;
    auto               process_term = query::term_processor(terms_file, stemmer);

    std::unordered_set<term_id_type> stopwords;
    if (stopwords_filename) {
        std::ifstream is(*stopwords_filename);
        io::for_each_line(is, [&](auto &&word) {
            if (auto processed_term = process_term(std::move(word)); process_term) {
                stopwords.insert(*processed_term);
            }
        });
    }

    auto push_query = [&](std::string const &query_line) {
        queries.push_back(parse_query(query_line, process_term, stopwords));
    };

    if (query_filename) {
        std::ifstream is(*query_filename);
        io::for_each_line(is, push_query);
    } else {
        io::for_each_line(std::cin, push_query);
    }

    /**/
    if (false) {  // NOLINT
#define LOOP_BODY(R, DATA, T)                                                           \
    }                                                                                   \
    else if (type == BOOST_PP_STRINGIZE(T)) {                                           \
        if (compressed) {                                                               \
            evaluate_queries<BOOST_PP_CAT(T, _index), wand_uniform_index>(              \
                index_filename, wand_data_filename, queries, thresholds_filename, type, \
                query_type, k, documents_file);                                         \
        } else {                                                                        \
            evaluate_queries<BOOST_PP_CAT(T, _index), wand_raw_index>(                  \
                index_filename, wand_data_filename, queries, thresholds_filename, type, \
                query_type, k, documents_file);                                         \
        }                                                                               \
        /**/

        BOOST_PP_SEQ_FOR_EACH(LOOP_BODY, _, PISA_INDEX_TYPES);
#undef LOOP_BODY
    } else {
        spdlog::error("Unknown type {}", type);
    }
}
