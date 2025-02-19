#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
// #include "gemma.h" // Adjust include path as necessary
#include <ctime>
#include <iostream>
#include <random>
#include <string>
#include <thread> // NOLINT
#include <vector>

#include "compression/compress.h"
#include "gemma.h" // Gemma
#include "util/app.h"
#include "util/args.h" // HasHelp
#include "hwy/base.h"
#include "hwy/contrib/thread_pool/thread_pool.h"
#include "hwy/highway.h"
#include "hwy/per_target.h"
#include "hwy/profiler.h"
#include "hwy/timer.h"

namespace py = pybind11;

namespace gcpp
{

    void ShowHelp(gcpp::LoaderArgs &loader, gcpp::InferenceArgs &inference,
                  gcpp::AppArgs &app)
    {
        fprintf(stderr,
                "\ngemma.cpp\n---------\n\nTo run gemma.cpp, you need to "
                "specify 3 required model loading arguments: --tokenizer, "
                "--compressed_weights, "
                "and --model.\n\nModel Loading Arguments\n\n");
        loader.Help();
        fprintf(stderr, "\nInference Arguments\n\n");
        inference.Help();
        fprintf(stderr, "\nApplication Arguments\n\n");
        app.Help();
        fprintf(stderr, "\n\n");
    }

    void ShowConfig(LoaderArgs &loader, InferenceArgs &inference, AppArgs &app)
    {
        loader.Print(app.verbosity);
        inference.Print(app.verbosity);
        app.Print(app.verbosity);

        if (app.verbosity >= 2)
        {
            time_t now = time(nullptr);
            char *dt = ctime(&now); // NOLINT
            std::cout << "Date & Time                   : " << dt
                      << "Prefill Token Batch Size      : " << gcpp::kPrefillBatchSize
                      << "\n"
                      << "Hardware concurrency          : "
                      << std::thread::hardware_concurrency() << std::endl
                      << "Instruction set               : "
                      << hwy::TargetName(hwy::DispatchedTarget()) << " ("
                      << hwy::VectorBytes() * 8 << " bits)"
                      << "\n"
                      << "Weight Type                   : "
                      << gcpp::TypeName(gcpp::WeightT()) << "\n"
                      << "EmbedderInput Type            : "
                      << gcpp::TypeName(gcpp::EmbedderInputT()) << "\n";
        }
    }

    void ReplGemma(gcpp::Gemma &model, hwy::ThreadPool &pool,
                   hwy::ThreadPool &inner_pool, const InferenceArgs &args,
                   int verbosity, const gcpp::AcceptFunc &accept_token)
    {
        PROFILER_ZONE("Gen.misc");
        int abs_pos = 0;     // absolute token index over all turns
        int current_pos = 0; // token index within the current turn
        int prompt_size{};

        std::mt19937 gen;
        if (args.deterministic)
        {
            gen.seed(42);
        }
        else
        {
            std::random_device rd;
            gen.seed(rd());
        }

        // callback function invoked for each generated token.
        auto stream_token = [&abs_pos, &current_pos, &args, &gen, &prompt_size,
                             tokenizer = &model.Tokenizer(),
                             verbosity](int token, float)
        {
            ++abs_pos;
            ++current_pos;
            if (current_pos < prompt_size)
            {
                std::cerr << "." << std::flush;
            }
            else if (token == gcpp::EOS_ID)
            {
                if (!args.multiturn)
                {
                    abs_pos = 0;
                    if (args.deterministic)
                    {
                        gen.seed(42);
                    }
                }
                if (verbosity >= 2)
                {
                    std::cout << "\n[ End ]" << std::endl;
                }
            }
            else
            {
                std::string token_text;
                HWY_ASSERT(tokenizer->Decode(std::vector<int>{token}, &token_text).ok());
                // +1 since position is incremented above
                if (current_pos == prompt_size + 1)
                {
                    // first token of response
                    token_text.erase(0, token_text.find_first_not_of(" \t\n"));
                    if (verbosity >= 1)
                    {
                        std::cout << std::endl
                                  << std::endl;
                    }
                }
                // TODO(austinvhuang): is explicit space necessary?
                std::cout << token_text << std::flush;
            }
            return true;
        };

        while (abs_pos < args.max_tokens)
        {
            std::string prompt_string;  
            std::vector<int> prompt;
            current_pos = 0;
            {
                PROFILER_ZONE("Gen.input");
                if (verbosity >= 1)
                {
                    std::cout << "> " << std::flush;
                }
                std::getline(std::cin, prompt_string);
            }

            if (std::cin.fail() || prompt_string == "%q" || prompt_string == "%Q")
            {
                return;
            }

            if (model.model_training == ModelTraining::GEMMA_IT)
            {
                // For instruction-tuned models: add control tokens.
                prompt_string = "<start_of_turn>user\n" + prompt_string +
                                "<end_of_turn>\n<start_of_turn>model\n";
                if (abs_pos > 0)
                {
                    // Prepend "<end_of_turn>" token if this is a multi-turn dialogue
                    // continuation.
                    prompt_string = "<end_of_turn>\n" + prompt_string;
                }
            }

            HWY_ASSERT(model.Tokenizer().Encode(prompt_string, &prompt).ok());

            // For both pre-trained and instruction-tuned models: prepend "<bos>" token
            // if needed.
            if (abs_pos == 0)
            {
                prompt.insert(prompt.begin(), 2);
            }

            prompt_size = prompt.size();

            std::cerr << std::endl
                      << "[ Reading prompt ] " << std::flush;

            const double time_start = hwy::platform::Now();
            GenerateGemma(model, args, prompt, abs_pos, pool, inner_pool, stream_token,
                          accept_token, gen, verbosity);
            const double time_end = hwy::platform::Now();
            const double tok_sec = current_pos / (time_end - time_start);
            if (verbosity >= 2)
            {
                std::cout << current_pos << " tokens (" << abs_pos << " total tokens)"
                          << std::endl
                          << tok_sec << " tokens / sec" << std::endl;
            }
            std::cout << std::endl
                      << std::endl;
        }
        std::cout
            << "max_tokens (" << args.max_tokens
            << ") exceeded. Use a larger value if desired using the --max_tokens "
            << "command line flag.\n";
    }

    void Run(LoaderArgs &loader, InferenceArgs &inference, AppArgs &app)
    {
        PROFILER_ZONE("Run.misc");

        hwy::ThreadPool inner_pool(0);
        hwy::ThreadPool pool(app.num_threads);
        // For many-core, pinning threads to cores helps.
        if (app.num_threads > 10)
        {
            PinThreadToCore(app.num_threads - 1); // Main thread

            pool.Run(0, pool.NumThreads(),
                     [](uint64_t /*task*/, size_t thread)
                     { PinThreadToCore(thread); });
        }

        gcpp::Gemma model(loader, pool);

        if (const char *error = inference.Validate())
        {
            ShowHelp(loader, inference, app);
            HWY_ABORT("\nInvalid args: %s", error);
        }

        if (app.verbosity >= 1)
        {
            static const std::string banner_ascii_art =
                "  __ _  ___ _ __ ___  _ __ ___   __ _   ___ _ __  _ __\n"
                " / _` |/ _ \\ '_ ` _ \\| '_ ` _ \\ / _` | / __| '_ \\| '_ \\\n"
                "| (_| |  __/ | | | | | | | | | | (_| || (__| |_) | |_) |\n"
                " \\__, |\\___|_| |_| |_|_| |_| |_|\\__,_(_)___| .__/| .__/\n"
                "  __/ |                                    | |   | |\n"
                " |___/                                     |_|   |_|";

            const std::string instructions =
                "*Usage*\n"
                "  Enter an instruction and press enter (%Q quits).\n\n"
                "*Examples*\n"
                "  - Write an email to grandma thanking her for the cookies.\n"
                "  - What are some historical attractions to visit around "
                "Massachusetts?\n"
                "  - Compute the nth fibonacci number in javascript.\n"
                "  - Write a standup comedy bit about GPU programming.\n";

            std::cout << "\033[2J\033[1;1H" // clear screen
                      << banner_ascii_art << "\n\n";
            ShowConfig(loader, inference, app);
            std::cout << "\n"
                      << instructions << "\n";
        }

        ReplGemma(model, pool, inner_pool, inference, app.verbosity,
                  /*accept_token=*/[](int)
                  { return true; });
    }

    std::string decode(gcpp::Gemma &model, hwy::ThreadPool &pool,
                   hwy::ThreadPool &inner_pool, const InferenceArgs &args,
                   int verbosity, const gcpp::AcceptFunc &accept_token, std::string &prompt_string) 
    {
        std::string generated_text;
        // Seed the random number generator
        std::random_device rd;
        std::mt19937 gen(rd());
        int prompt_size{};
        if (model.model_training == ModelTraining::GEMMA_IT)
            {
                // For instruction-tuned models: add control tokens.
                prompt_string = "<start_of_turn>user\n" + prompt_string +
                                "<end_of_turn>\n<start_of_turn>model\n";
            }
        // Encode the prompt string into tokens
        std::vector<int> prompt;
        HWY_ASSERT(model.Tokenizer().Encode(prompt_string, &prompt).ok());
        // Placeholder for generated token IDs
        std::vector<int> generated_tokens;
        // Define lambda for token decoding
        StreamFunc stream_token = [&generated_tokens](int token, float /* probability */) -> bool {
            generated_tokens.push_back(token);
            return true; // Continue generating
        };
        // Decode tokens
        prompt_size = prompt.size();    
        GenerateGemma(model, args, prompt, /*start_pos=*/0, pool, inner_pool, stream_token, accept_token, gen, verbosity);
        HWY_ASSERT(model.Tokenizer().Decode(generated_tokens, &generated_text).ok());
        generated_text = generated_text.substr(prompt_string.size());

    return generated_text;
    }

    std::string completion(LoaderArgs &loader, InferenceArgs &inference, AppArgs &app, std::string &prompt_string)
    {
        hwy::ThreadPool inner_pool(0);
        hwy::ThreadPool pool(app.num_threads);
        if (app.num_threads > 10)
        {
            PinThreadToCore(app.num_threads - 1); // Main thread

            pool.Run(0, pool.NumThreads(),
                     [](uint64_t /*task*/, size_t thread)
                     { PinThreadToCore(thread); });
        }
        gcpp::Gemma model(loader, pool);
        return decode(model, pool, inner_pool, inference, app.verbosity, /*accept_token=*/[](int)
                  { return true; }, prompt_string);

    }

} // namespace gcpp

void chat_base(int argc, char **argv)
{
    {
        PROFILER_ZONE("Startup.misc");

        gcpp::LoaderArgs loader(argc, argv);
        gcpp::InferenceArgs inference(argc, argv);
        gcpp::AppArgs app(argc, argv);

        if (gcpp::HasHelp(argc, argv))
        {
            ShowHelp(loader, inference, app);
            // return 0;
        }

        if (const char *error = loader.Validate())
        {
            ShowHelp(loader, inference, app);
            HWY_ABORT("\nInvalid args: %s", error);
        }

        gcpp::Run(loader, inference, app);
    }
    PROFILER_PRINT_RESULTS(); // Must call outside the zone above.
    // return 1;
}
std::string completion_base(int argc, char **argv)
{   
    gcpp::LoaderArgs loader(argc, argv);
    gcpp::InferenceArgs inference(argc, argv);
    gcpp::AppArgs app(argc, argv);
    std::string prompt_string = argv[argc-1];
    return gcpp::completion(loader, inference, app, prompt_string);
}
std::string completion_base_wrapper(const std::vector<std::string> &args,std::string &prompt_string)
{
    int argc = args.size() + 2; // +1 for the program name
    std::vector<char *> argv_vec;
    argv_vec.reserve(argc);

    argv_vec.push_back(const_cast<char *>("pygemma"));

    for (const auto &arg : args)
    {
        argv_vec.push_back(const_cast<char *>(arg.c_str()));
    }
    argv_vec.push_back(const_cast<char *>(prompt_string.c_str()));
    char **argv = argv_vec.data();
    return completion_base(argc, argv);
}
void show_help_wrapper()
{
    // Assuming ShowHelp does not critically depend on argv content
    gcpp::LoaderArgs loader(0, nullptr);
    gcpp::InferenceArgs inference(0, nullptr);
    gcpp::AppArgs app(0, nullptr);

    ShowHelp(loader, inference, app);
}

std::string chat_base_wrapper(const std::vector<std::string> &args)
{
    int argc = args.size() + 1; // +1 for the program name
    std::vector<char *> argv_vec;
    argv_vec.reserve(argc);
    argv_vec.push_back(const_cast<char *>("pygemma"));

    for (const auto &arg : args)
    {
        argv_vec.push_back(const_cast<char *>(arg.c_str()));
    }

    char **argv = argv_vec.data();

    chat_base(argc, argv);
}


PYBIND11_MODULE(pygemma, m)
{
    m.doc() = "Pybind11 integration for chat_base function";
    m.def("chat_base", &chat_base_wrapper, "A wrapper for the chat_base function accepting Python list of strings as arguments");
    m.def("show_help", &show_help_wrapper, "A wrapper for show_help function");
    m.def("completion", &completion_base_wrapper, "A wrapper for inference function");
}
