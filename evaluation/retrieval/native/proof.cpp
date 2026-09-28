// Embeds or reranks through the GenAI C++ pipelines. Prints JSON lines for the Python harness.
//
//   proof embed  <model_dir> <cls|mean|last> <max_length> <texts.txt> [query_instruction]
//   proof rerank <model_dir> <max_length> <query> <texts.txt> [repeats]
//
// texts.txt: one UTF-8 text per line. embed prints one JSON array per line, then "query" and
// the first line's query embedding. rerank prints [index, score] pairs.

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "openvino/genai/rag/text_embedding_pipeline.hpp"
#include "openvino/genai/rag/text_rerank_pipeline.hpp"

namespace {

std::vector<std::string> ReadLines(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

void PrintVector(const std::vector<float>& v) {
    std::cout << '[';
    for (size_t i = 0; i < v.size(); ++i) std::cout << (i ? "," : "") << v[i];
    std::cout << "]\n";
}

int Embed(int argc, char* argv[]) {
    if (argc < 6) throw std::runtime_error("usage: proof embed <model_dir> <cls|mean|last> <max_length> <texts.txt> [query_instruction]");
    using Pipeline = ov::genai::TextEmbeddingPipeline;
    Pipeline::Config config;
    const std::string pooling = argv[3];
    config.pooling_type = pooling == "cls" ? Pipeline::PoolingType::CLS
                        : pooling == "mean" ? Pipeline::PoolingType::MEAN
                        : Pipeline::PoolingType::LAST_TOKEN;
    config.normalize = true;
    config.max_length = static_cast<size_t>(std::stoul(argv[4]));
    if (pooling == "last") config.padding_side = "left";
    if (argc > 6) config.query_instruction = argv[6];
    const auto texts = ReadLines(argv[5]);

    Pipeline pipeline(argv[2], "CPU", config);
    const auto documents = pipeline.embed_documents(texts);
    std::cout << std::setprecision(8);
    for (const auto& v : std::get<std::vector<std::vector<float>>>(documents)) PrintVector(v);
    std::cout << "query\n";
    PrintVector(std::get<std::vector<float>>(pipeline.embed_query(texts.front())));
    return 0;
}

// repeats > 1: successive texts become queries. Only the last result is printed
int Rerank(int argc, char* argv[]) {
    if (argc < 6) throw std::runtime_error("usage: proof rerank <model_dir> <max_length> <query> <texts.txt> [repeats]");
    ov::genai::TextRerankPipeline::Config config;
    config.max_length = static_cast<size_t>(std::stoul(argv[3]));
    const auto texts = ReadLines(argv[5]);
    config.top_n = texts.size();
    const size_t repeats = argc > 6 ? std::stoul(argv[6]) : 1;

    ov::genai::TextRerankPipeline pipeline(argv[2], "CPU", config);
    std::vector<std::pair<size_t, float>> result;
    for (size_t i = 0; i < repeats; ++i) {
        const std::string& query = i == 0 ? std::string(argv[4]) : texts[i % texts.size()];
        result = pipeline.rerank(query, texts);
        std::cerr << "iter " << i << '\n';
    }
    std::cout << std::setprecision(8);
    for (const auto& [index, score] : result) {
        std::cout << '[' << index << ',' << score << "]\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) try {
    if (argc < 2) throw std::runtime_error("usage: proof embed|rerank ...");
    const std::string mode = argv[1];
    if (mode == "embed") return Embed(argc, argv);
    if (mode == "rerank") return Rerank(argc, argv);
    throw std::runtime_error("unknown mode " + mode);
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
