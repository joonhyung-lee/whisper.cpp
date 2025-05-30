/*

# For macOS
brew install portaudio
# For Ubuntu/Debian
sudo apt-get install libportaudio2 portaudio19-dev
# For Fedora/RHEL
sudo dnf install portaudio portaudio-devel

# build the project
cmake -B build
cmake --build build --config Release

# run the project
./build/bin/mic-cli --model models/ggml-base.bin --vad -vm ./models/silero-v5.1.2-ggml.bin -f - -l ko
*/
#include "common.h"
#include "common-whisper.h"

#include "whisper.h"
#include "grammar-parser.h"

#include <cmath>
#include <fstream>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <cfloat>
#include <csignal>
#include <atomic>

#include <portaudio.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// helper function to replace substrings
static void replace_all(std::string & s, const std::string & search, const std::string & replace) {
    for (size_t pos = 0; ; pos += replace.length()) {
        pos = s.find(search, pos);
        if (pos == std::string::npos) break;
        s.erase(pos, search.length());
        s.insert(pos, replace);
    }
}

// command-line parameters
struct whisper_params {
    int32_t n_threads     = std::min(4, (int32_t) std::thread::hardware_concurrency());
    int32_t n_processors  = 1;
    int32_t offset_t_ms   = 0;
    int32_t offset_n      = 0;
    int32_t duration_ms   = 0;
    int32_t progress_step = 5;
    int32_t max_context   = -1;
    int32_t max_len       = 0;
    int32_t best_of       = whisper_full_default_params(WHISPER_SAMPLING_GREEDY).greedy.best_of;
    int32_t beam_size     = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH).beam_search.beam_size;
    int32_t audio_ctx     = 0;

    float word_thold      =  0.01f;
    float entropy_thold   =  2.40f;
    float logprob_thold   = -1.00f;
    float no_speech_thold =  0.6f;
    float grammar_penalty = 100.0f;
    float temperature     = 0.0f;
    float temperature_inc = 0.2f;

    bool debug_mode      = false;
    bool translate       = false;
    bool detect_language = false;
    bool diarize         = false;
    bool tinydiarize     = false;
    bool split_on_word   = false;
    bool no_fallback     = false;
    bool output_txt      = false;
    bool output_vtt      = false;
    bool output_srt      = false;
    bool output_wts      = false;
    bool output_csv      = false;
    bool output_jsn      = false;
    bool output_jsn_full = false;
    bool output_lrc      = false;
    bool no_prints       = false;
    bool print_special   = false;
    bool print_colors    = false;
    bool print_progress  = false;
    bool no_timestamps   = false;
    bool log_score       = false;
    bool use_gpu         = true;
    bool flash_attn      = false;
    bool suppress_nst    = false;

    // Audio device settings
    int device_index     = -1;  // -1 means default device
    int sample_rate      = 16000;
    int frames_per_buffer = 1024;
    float record_duration = 0.0f; // 0 means continuous recording until stopped

    std::string language  = "en";
    std::string prompt;
    std::string font_path = "/System/Library/Fonts/Supplemental/Courier New Bold.ttf";
    std::string model     = "models/ggml-base.en.bin";
    std::string grammar;
    std::string grammar_rule;

    // [TDRZ] speaker turn string
    std::string tdrz_speaker_turn = " [SPEAKER_TURN]"; // TODO: set from command line

    // A regular expression that matches tokens to suppress
    std::string suppress_regex;

    std::string openvino_encode_device = "CPU";

    std::string dtw = "";

    std::vector<std::string> fname_inp = {};
    std::vector<std::string> fname_out = {};

    grammar_parser::parse_state grammar_parsed;

    // Voice Activity Detection (VAD) parameters
    bool        vad           = false;
    std::string vad_model     = "";
    float       vad_threshold = 0.5f;
    int         vad_min_speech_duration_ms = 250;
    int         vad_min_silence_duration_ms = 100;
    float       vad_max_speech_duration_s = FLT_MAX;
    int         vad_speech_pad_ms = 30;
    float       vad_samples_overlap = 0.1f;
};

// Global variables to handle interruption
std::atomic<bool> is_running{true};

// Signal handler for clean termination
void signalHandler(int signum) {
    printf("\nInterrupt signal received. Stopping...\n");
    is_running = false;
}

// PortAudio callback function
typedef struct {
    std::vector<float> *recorded_data;
    whisper_context *ctx;
    whisper_params *params;
    bool processing;
    std::vector<float> buffer;
    size_t buffer_size;
    size_t buffer_index;
} paUserData;

static int recordCallback(const void *inputBuffer, void *outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo *timeInfo,
                         PaStreamCallbackFlags statusFlags,
                         void *userData) {
    paUserData *data = (paUserData*)userData;
    const float *in = (const float*)inputBuffer;
    (void)outputBuffer; // Unused
    (void)timeInfo;
    (void)statusFlags;

    if (in == NULL) {
        // No input data - fill with zeros
        for (unsigned long i = 0; i < framesPerBuffer; i++) {
            data->recorded_data->push_back(0.0f);
        }
    } else {
        // Copy input data to our buffer
        for (unsigned long i = 0; i < framesPerBuffer; i++) {
            data->recorded_data->push_back(in[i]);
            
            // Also add to the processing buffer
            if (data->buffer_index < data->buffer_size) {
                data->buffer[data->buffer_index++] = in[i];
            }
        }
        
        // If buffer is full and we're not already processing
        if (data->buffer_index >= data->buffer_size && !data->processing) {
            // Process the audio buffer in a separate thread
            data->processing = true;
            std::thread([data]() {
                // Create a copy of the buffer for processing
                std::vector<float> process_buffer(data->buffer.begin(), data->buffer.end());
                
                // Process audio with whisper
                whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                
                wparams.print_realtime   = true;
                wparams.print_progress   = false;
                wparams.print_timestamps = !data->params->no_timestamps;
                wparams.print_special    = data->params->print_special;
                wparams.translate        = data->params->translate;
                wparams.language         = data->params->language.c_str();
                wparams.detect_language  = data->params->detect_language;
                wparams.n_threads        = data->params->n_threads;
                wparams.offset_ms        = 0;
                wparams.duration_ms      = 0;
                
                wparams.token_timestamps = false;
                wparams.thold_pt         = data->params->word_thold;
                wparams.max_len          = data->params->max_len;
                wparams.split_on_word    = data->params->split_on_word;
                
                wparams.suppress_nst     = data->params->suppress_nst;
                
                // VAD options
                wparams.vad              = data->params->vad;
                if (data->params->vad) {
                    wparams.vad_model_path = data->params->vad_model.c_str();
                    wparams.vad_params.threshold = data->params->vad_threshold;
                    wparams.vad_params.min_speech_duration_ms = data->params->vad_min_speech_duration_ms;
                    wparams.vad_params.min_silence_duration_ms = data->params->vad_min_silence_duration_ms;
                    wparams.vad_params.max_speech_duration_s = data->params->vad_max_speech_duration_s;
                    wparams.vad_params.speech_pad_ms = data->params->vad_speech_pad_ms;
                    wparams.vad_params.samples_overlap = data->params->vad_samples_overlap;
                }
                
                if (whisper_full(data->ctx, wparams, process_buffer.data(), process_buffer.size()) != 0) {
                    fprintf(stderr, "Failed to process audio\n");
                }
                
                // Reset the buffer for the next chunk
                data->buffer_index = 0;
                data->processing = false;
            }).detach();
        }
    }

    return is_running ? paContinue : paComplete;
}

static void whisper_print_usage(int argc, char ** argv, const whisper_params & params);

static char * whisper_param_turn_lowercase(char * in){
    int string_len = strlen(in);
    for (int i = 0; i < string_len; i++){
        *(in+i) = tolower((unsigned char)*(in+i));
    }
    return in;
}

static char * requires_value_error(const std::string & arg) {
    fprintf(stderr, "error: argument %s requires value\n", arg.c_str());
    exit(0);
}

static bool whisper_params_parse(int argc, char ** argv, whisper_params & params) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-"){
            params.fname_inp.push_back(arg);
            continue;
        }

        if (arg[0] != '-') {
            params.fname_inp.push_back(arg);
            continue;
        }

        if (arg == "-h" || arg == "--help") {
            whisper_print_usage(argc, argv, params);
            exit(0);
        }
        #define ARGV_NEXT (((i + 1) < argc) ? argv[++i] : requires_value_error(arg))
        else if (arg == "-t"    || arg == "--threads")         { params.n_threads       = std::stoi(ARGV_NEXT); }
        else if (arg == "-p"    || arg == "--processors")      { params.n_processors    = std::stoi(ARGV_NEXT); }
        else if (arg == "-ot"   || arg == "--offset-t")        { params.offset_t_ms     = std::stoi(ARGV_NEXT); }
        else if (arg == "-on"   || arg == "--offset-n")        { params.offset_n        = std::stoi(ARGV_NEXT); }
        else if (arg == "-d"    || arg == "--duration")        { params.duration_ms     = std::stoi(ARGV_NEXT); }
        else if (arg == "-mc"   || arg == "--max-context")     { params.max_context     = std::stoi(ARGV_NEXT); }
        else if (arg == "-ml"   || arg == "--max-len")         { params.max_len         = std::stoi(ARGV_NEXT); }
        else if (arg == "-bo"   || arg == "--best-of")         { params.best_of         = std::stoi(ARGV_NEXT); }
        else if (arg == "-bs"   || arg == "--beam-size")       { params.beam_size       = std::stoi(ARGV_NEXT); }
        else if (arg == "-ac"   || arg == "--audio-ctx")       { params.audio_ctx       = std::stoi(ARGV_NEXT); }
        else if (arg == "-wt"   || arg == "--word-thold")      { params.word_thold      = std::stof(ARGV_NEXT); }
        else if (arg == "-et"   || arg == "--entropy-thold")   { params.entropy_thold   = std::stof(ARGV_NEXT); }
        else if (arg == "-lpt"  || arg == "--logprob-thold")   { params.logprob_thold   = std::stof(ARGV_NEXT); }
        else if (arg == "-nth"  || arg == "--no-speech-thold") { params.no_speech_thold = std::stof(ARGV_NEXT); }
        else if (arg == "-tp"   || arg == "--temperature")     { params.temperature     = std::stof(ARGV_NEXT); }
        else if (arg == "-tpi"  || arg == "--temperature-inc") { params.temperature_inc = std::stof(ARGV_NEXT); }
        else if (arg == "-debug"|| arg == "--debug-mode")      { params.debug_mode      = true; }
        else if (arg == "-tr"   || arg == "--translate")       { params.translate       = true; }
        else if (arg == "-di"   || arg == "--diarize")         { params.diarize         = true; }
        else if (arg == "-tdrz" || arg == "--tinydiarize")     { params.tinydiarize     = true; }
        else if (arg == "-sow"  || arg == "--split-on-word")   { params.split_on_word   = true; }
        else if (arg == "-nf"   || arg == "--no-fallback")     { params.no_fallback     = true; }
        else if (arg == "-otxt" || arg == "--output-txt")      { params.output_txt      = true; }
        else if (arg == "-ovtt" || arg == "--output-vtt")      { params.output_vtt      = true; }
        else if (arg == "-osrt" || arg == "--output-srt")      { params.output_srt      = true; }
        else if (arg == "-owts" || arg == "--output-words")    { params.output_wts      = true; }
        else if (arg == "-olrc" || arg == "--output-lrc")      { params.output_lrc      = true; }
        else if (arg == "-fp"   || arg == "--font-path")       { params.font_path       = ARGV_NEXT; }
        else if (arg == "-ocsv" || arg == "--output-csv")      { params.output_csv      = true; }
        else if (arg == "-oj"   || arg == "--output-json")     { params.output_jsn      = true; }
        else if (arg == "-ojf"  || arg == "--output-json-full"){ params.output_jsn_full = params.output_jsn = true; }
        else if (arg == "-of FNAME, --output-file FNAME" || arg == "-of FNAME" || arg == "-of FNAME") { params.fname_out.emplace_back(ARGV_NEXT); }
        else if (arg == "-np"   || arg == "--no-prints")       { params.no_prints       = true; }
        else if (arg == "-ps"   || arg == "--print-special")   { params.print_special   = true; }
        else if (arg == "-pc"   || arg == "--print-colors")    { params.print_colors    = true; }
        else if (arg == "-pp"   || arg == "--print-progress")  { params.print_progress  = true; }
        else if (arg == "-nt"   || arg == "--no-timestamps")   { params.no_timestamps   = true; }
        else if (arg == "-l"    || arg == "--language")        { params.language        = whisper_param_turn_lowercase(ARGV_NEXT); }
        else if (arg == "-dl"   || arg == "--detect-language") { params.detect_language = true; }
        else if (                  arg == "--prompt")          { params.prompt          = ARGV_NEXT; }
        else if (arg == "-m"    || arg == "--model")           { params.model           = ARGV_NEXT; }
        else if (arg == "-f"    || arg == "--file")            { params.fname_inp.emplace_back(ARGV_NEXT); }
        else if (arg == "-oved D,   --ov-e-device DNAME" || arg == "-oved" || arg == "-oved D" || arg == "-oved DNAME") { params.openvino_encode_device = ARGV_NEXT; }
        else if (arg == "-dtw MODEL --dtw MODEL" || arg == "-dtw" || arg == "-dtw MODEL") { params.dtw = ARGV_NEXT; }
        else if (arg == "-ls"   || arg == "--log-score")       { params.log_score       = true; }
        else if (arg == "-ng"   || arg == "--no-gpu")          { params.use_gpu         = false; }
        else if (arg == "-fa"   || arg == "--flash-attn")      { params.flash_attn      = true; }
        else if (arg == "-sns"  || arg == "--suppress-nst")    { params.suppress_nst    = true; }
        else if (                  arg == "--suppress-regex")  { params.suppress_regex  = ARGV_NEXT; }
        else if (                  arg == "--grammar")         { params.grammar         = ARGV_NEXT; }
        else if (                  arg == "--grammar-rule")    { params.grammar_rule    = ARGV_NEXT; }
        else if (                  arg == "--grammar-penalty N") { params.grammar_penalty = std::stof(ARGV_NEXT); }
        // Voice Activity Detection (VAD)
        else if (arg == "-v"    || arg == "--vad")                         { params.vad                         = true; }
        else if (arg == "-vm FNAME, --vad-model FNAME" || arg == "-vm" || arg == "-vm FNAME" || arg == "-vm FNAME") { params.vad_model = ARGV_NEXT; }
        else if (arg == "-vt N,     --vad-threshold N" || arg == "-vt" || arg == "-vt N" || arg == "-vt N") { params.vad_threshold = std::stof(ARGV_NEXT); }
        else if (arg == "-vsd"  || arg == "--vad-min-speech-duration-ms" || arg == "-vsd" || arg == "-vsd N") { params.vad_min_speech_duration_ms = std::stoi(ARGV_NEXT); }
        else if (arg == "-vsd"  || arg == "--vad-min-silence-duration-ms" || arg == "-vsd" || arg == "-vsd N") { params.vad_min_silence_duration_ms = std::stoi(ARGV_NEXT); }
        else if (arg == "-vmsd N,   --vad-max-speech-duration-s" || arg == "-vmsd" || arg == "-vmsd N") { params.vad_max_speech_duration_s = std::stof(ARGV_NEXT); }
        else if (arg == "-vp"   || arg == "--vad-speech-pad-ms" || arg == "-vp" || arg == "-vp N") { params.vad_speech_pad_ms = std::stoi(ARGV_NEXT); }
        else if (arg == "-vo"   || arg == "--vad-samples-overlap" || arg == "-vo" || arg == "-vo N") { params.vad_samples_overlap = std::stof(ARGV_NEXT); }
        else if (arg == "-d" || arg == "--device" || arg == "-d INDEX" || arg == "-d INDEX") { params.device_index = std::stoi(ARGV_NEXT); }
        else if (arg == "-sr" || arg == "--sample-rate" || arg == "-sr N" || arg == "-sr N") { params.sample_rate = std::stoi(ARGV_NEXT); }
        else if (arg == "-rd" || arg == "--record-duration" || arg == "-rd N" || arg == "-rd N") { params.record_duration = std::stof(ARGV_NEXT); }
        else {
            fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            whisper_print_usage(argc, argv, params);
            exit(0);
        }
    }

    return true;
}

static void whisper_print_usage(int /*argc*/, char ** argv, const whisper_params & params) {
    fprintf(stderr, "\n");
    fprintf(stderr, "usage: %s [options] file0 file1 ...\n", argv[0]);
    fprintf(stderr, "supported audio formats: flac, mp3, ogg, wav\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -h,        --help              [default] show this help message and exit\n");
    fprintf(stderr, "  -t N,      --threads N         [%-7d] number of threads to use during computation\n",    params.n_threads);
    fprintf(stderr, "  -p N,      --processors N      [%-7d] number of processors to use during computation\n", params.n_processors);
    fprintf(stderr, "  -ot N,     --offset-t N        [%-7d] time offset in milliseconds\n",                    params.offset_t_ms);
    fprintf(stderr, "  -on N,     --offset-n N        [%-7d] segment index offset\n",                           params.offset_n);
    fprintf(stderr, "  -d  N,     --duration N        [%-7d] duration of audio to process in milliseconds\n",   params.duration_ms);
    fprintf(stderr, "  -mc N,     --max-context N     [%-7d] maximum number of text context tokens to store\n", params.max_context);
    fprintf(stderr, "  -ml N,     --max-len N         [%-7d] maximum segment length in characters\n",           params.max_len);
    fprintf(stderr, "  -sow,      --split-on-word     [%-7s] split on word rather than on token\n",             params.split_on_word ? "true" : "false");
    fprintf(stderr, "  -bo N,     --best-of N         [%-7d] number of best candidates to keep\n",              params.best_of);
    fprintf(stderr, "  -bs N,     --beam-size N       [%-7d] beam size for beam search\n",                      params.beam_size);
    fprintf(stderr, "  -ac N,     --audio-ctx N       [%-7d] audio context size (0 - all)\n",                   params.audio_ctx);
    fprintf(stderr, "  -wt N,     --word-thold N      [%-7.2f] word timestamp probability threshold\n",         params.word_thold);
    fprintf(stderr, "  -et N,     --entropy-thold N   [%-7.2f] entropy threshold for decoder fail\n",           params.entropy_thold);
    fprintf(stderr, "  -lpt N,    --logprob-thold N   [%-7.2f] log probability threshold for decoder fail\n",   params.logprob_thold);
    fprintf(stderr, "  -nth N,    --no-speech-thold N [%-7.2f] no speech threshold\n",                          params.no_speech_thold);
    fprintf(stderr, "  -tp,       --temperature N     [%-7.2f] The sampling temperature, between 0 and 1\n",    params.temperature);
    fprintf(stderr, "  -tpi,      --temperature-inc N [%-7.2f] The increment of temperature, between 0 and 1\n",params.temperature_inc);
    fprintf(stderr, "  -debug,    --debug-mode        [%-7s] enable debug mode (eg. dump log_mel)\n",           params.debug_mode ? "true" : "false");
    fprintf(stderr, "  -tr,       --translate         [%-7s] translate from source language to english\n",      params.translate ? "true" : "false");
    fprintf(stderr, "  -di,       --diarize           [%-7s] stereo audio diarization\n",                       params.diarize ? "true" : "false");
    fprintf(stderr, "  -tdrz,     --tinydiarize       [%-7s] enable tinydiarize (requires a tdrz model)\n",     params.tinydiarize ? "true" : "false");
    fprintf(stderr, "  -nf,       --no-fallback       [%-7s] do not use temperature fallback while decoding\n", params.no_fallback ? "true" : "false");
    fprintf(stderr, "  -otxt,     --output-txt        [%-7s] output result in a text file\n",                   params.output_txt ? "true" : "false");
    fprintf(stderr, "  -ovtt,     --output-vtt        [%-7s] output result in a vtt file\n",                    params.output_vtt ? "true" : "false");
    fprintf(stderr, "  -osrt,     --output-srt        [%-7s] output result in a srt file\n",                    params.output_srt ? "true" : "false");
    fprintf(stderr, "  -olrc,     --output-lrc        [%-7s] output result in a lrc file\n",                    params.output_lrc ? "true" : "false");
    fprintf(stderr, "  -owts,     --output-words      [%-7s] output script for generating karaoke video\n",     params.output_wts ? "true" : "false");
    fprintf(stderr, "  -fp,       --font-path         [%-7s] path to a monospace font for karaoke video\n",     params.font_path.c_str());
    fprintf(stderr, "  -ocsv,     --output-csv        [%-7s] output result in a CSV file\n",                    params.output_csv ? "true" : "false");
    fprintf(stderr, "  -oj,       --output-json       [%-7s] output result in a JSON file\n",                   params.output_jsn ? "true" : "false");
    fprintf(stderr, "  -ojf,      --output-json-full  [%-7s] include more information in the JSON file\n",      params.output_jsn_full ? "true" : "false");
    fprintf(stderr, "  -of FNAME, --output-file FNAME [%-7s] output file path (without file extension)\n",      "");
    fprintf(stderr, "  -np,       --no-prints         [%-7s] do not print anything other than the results\n",   params.no_prints ? "true" : "false");
    fprintf(stderr, "  -ps,       --print-special     [%-7s] print special tokens\n",                           params.print_special ? "true" : "false");
    fprintf(stderr, "  -pc,       --print-colors      [%-7s] print colors\n",                                   params.print_colors ? "true" : "false");
    fprintf(stderr, "  -pp,       --print-progress    [%-7s] print progress\n",                                 params.print_progress ? "true" : "false");
    fprintf(stderr, "  -nt,       --no-timestamps     [%-7s] do not print timestamps\n",                        params.no_timestamps ? "true" : "false");
    fprintf(stderr, "  -l LANG,   --language LANG     [%-7s] spoken language ('auto' for auto-detect)\n",       params.language.c_str());
    fprintf(stderr, "  -dl,       --detect-language   [%-7s] exit after automatically detecting language\n",    params.detect_language ? "true" : "false");
    fprintf(stderr, "             --prompt PROMPT     [%-7s] initial prompt (max n_text_ctx/2 tokens)\n",       params.prompt.c_str());
    fprintf(stderr, "  -m FNAME,  --model FNAME       [%-7s] model path\n",                                     params.model.c_str());
    fprintf(stderr, "  -f FNAME,  --file FNAME        [%-7s] input audio file path\n",                            "");
    fprintf(stderr, "  -oved D,   --ov-e-device DNAME [%-7s] the OpenVINO device used for encode inference\n",  params.openvino_encode_device.c_str());
    fprintf(stderr, "  -dtw MODEL --dtw MODEL         [%-7s] compute token-level timestamps\n",                 params.dtw.c_str());
    fprintf(stderr, "  -ls,       --log-score         [%-7s] log best decoder scores of tokens\n",              params.log_score?"true":"false");
    fprintf(stderr, "  -ng,       --no-gpu            [%-7s] disable GPU\n",                                    params.use_gpu ? "false" : "true");
    fprintf(stderr, "  -fa,       --flash-attn        [%-7s] flash attention\n",                                params.flash_attn ? "true" : "false");
    fprintf(stderr, "  -sns,      --suppress-nst      [%-7s] suppress non-speech tokens\n",                     params.suppress_nst ? "true" : "false");
    fprintf(stderr, "  --suppress-regex REGEX         [%-7s] regular expression matching tokens to suppress\n", params.suppress_regex.c_str());
    fprintf(stderr, "  --grammar GRAMMAR              [%-7s] GBNF grammar to guide decoding\n",                 params.grammar.c_str());
    fprintf(stderr, "  --grammar-rule RULE            [%-7s] top-level GBNF grammar rule name\n",               params.grammar_rule.c_str());
    fprintf(stderr, "  --grammar-penalty N            [%-7.1f] scales down logits of nongrammar tokens\n",      params.grammar_penalty);
    // Voice Activity Detection (VAD) parameters
    fprintf(stderr, "\nVoice Activity Detection (VAD) options:\n");
    fprintf(stderr, "  -v,        --vad                           [%-7s] enable Voice Activity Detection (VAD)\n",            params.vad ? "true" : "false");
    fprintf(stderr, "  -vm FNAME, --vad-model FNAME               [%-7s] VAD model path\n",                                   params.vad_model.c_str());
    fprintf(stderr, "  -vt N,     --vad-threshold N               [%-7.2f] VAD threshold for speech recognition\n",           params.vad_threshold);
    fprintf(stderr, "  -vspd N,   --vad-min-speech-duration-ms  N [%-7d] VAD min speech duration (0.0-1.0)\n",                params.vad_min_speech_duration_ms);
    fprintf(stderr, "  -vsd N,    --vad-min-silence-duration-ms N [%-7d] VAD min silence duration (to split segments)\n",      params.vad_min_silence_duration_ms);
    fprintf(stderr, "  -vmsd N,   --vad-max-speech-duration-s   N [%-7s] VAD max speech duration (auto-split longer)\n",      params.vad_max_speech_duration_s == FLT_MAX ?
                                                                                                                                  std::string("FLT_MAX").c_str() :
                                                                                                                                  std::to_string(params.vad_max_speech_duration_s).c_str());
    fprintf(stderr, "  -vp N,     --vad-speech-pad-ms           N [%-7d] VAD speech padding (extend segments)\n",             params.vad_speech_pad_ms);
    fprintf(stderr, "  -vo N,     --vad-samples-overlap         N [%-7.2f] VAD samples overlap (seconds between segments)\n", params.vad_samples_overlap);
    fprintf(stderr, "\n");
}

struct whisper_print_user_data {
    const whisper_params * params;

    const std::vector<std::vector<float>> * pcmf32s;
    int progress_prev;
};

static std::string estimate_diarization_speaker(std::vector<std::vector<float>> pcmf32s, int64_t t0, int64_t t1, bool id_only = false) {
    std::string speaker = "";
    const int64_t n_samples = pcmf32s[0].size();

    const int64_t is0 = timestamp_to_sample(t0, n_samples, WHISPER_SAMPLE_RATE);
    const int64_t is1 = timestamp_to_sample(t1, n_samples, WHISPER_SAMPLE_RATE);

    double energy0 = 0.0f;
    double energy1 = 0.0f;

    for (int64_t j = is0; j < is1; j++) {
        energy0 += fabs(pcmf32s[0][j]);
        energy1 += fabs(pcmf32s[1][j]);
    }

    if (energy0 > 1.1*energy1) {
        speaker = "0";
    } else if (energy1 > 1.1*energy0) {
        speaker = "1";
    } else {
        speaker = "?";
    }

    //printf("is0 = %lld, is1 = %lld, energy0 = %f, energy1 = %f, speaker = %s\n", is0, is1, energy0, energy1, speaker.c_str());

    if (!id_only) {
        speaker.insert(0, "(speaker ");
        speaker.append(")");
    }

    return speaker;
}

static void whisper_print_progress_callback(struct whisper_context * /*ctx*/, struct whisper_state * /*state*/, int progress, void * user_data) {
    int progress_step = ((whisper_print_user_data *) user_data)->params->progress_step;
    int * progress_prev  = &(((whisper_print_user_data *) user_data)->progress_prev);
    if (progress >= *progress_prev + progress_step) {
        *progress_prev += progress_step;
        fprintf(stderr, "%s: progress = %3d%%\n", __func__, progress);
    }
}

static void whisper_print_segment_callback(struct whisper_context * ctx, struct whisper_state * /*state*/, int n_new, void * user_data) {
    const auto & params  = *((whisper_print_user_data *) user_data)->params;
    const auto & pcmf32s = *((whisper_print_user_data *) user_data)->pcmf32s;

    const int n_segments = whisper_full_n_segments(ctx);

    std::string speaker = "";

    int64_t t0 = 0;
    int64_t t1 = 0;

    // print the last n_new segments
    const int s0 = n_segments - n_new;

    if (s0 == 0) {
        printf("\n");
    }

    for (int i = s0; i < n_segments; i++) {
        if (!params.no_timestamps || params.diarize) {
            t0 = whisper_full_get_segment_t0(ctx, i);
            t1 = whisper_full_get_segment_t1(ctx, i);
        }

        if (!params.no_timestamps) {
            printf("[%s --> %s]  ", to_timestamp(t0).c_str(), to_timestamp(t1).c_str());
        }

        if (params.diarize && pcmf32s.size() == 2) {
            speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
        }

        if (params.print_colors) {
            for (int j = 0; j < whisper_full_n_tokens(ctx, i); ++j) {
                if (params.print_special == false) {
                    const whisper_token id = whisper_full_get_token_id(ctx, i, j);
                    if (id >= whisper_token_eot(ctx)) {
                        continue;
                    }
                }

                const char * text = whisper_full_get_token_text(ctx, i, j);
                const float  p    = whisper_full_get_token_p   (ctx, i, j);

                const int col = std::max(0, std::min((int) k_colors.size() - 1, (int) (std::pow(p, 3)*float(k_colors.size()))));

                printf("%s%s%s%s", speaker.c_str(), k_colors[col].c_str(), text, "\033[0m");
            }
        } else {
            const char * text = whisper_full_get_segment_text(ctx, i);

            printf("%s%s", speaker.c_str(), text);
        }

        if (params.tinydiarize) {
            if (whisper_full_get_segment_speaker_turn_next(ctx, i)) {
                printf("%s", params.tdrz_speaker_turn.c_str());
            }
        }

        // with timestamps or speakers: each segment on new line
        if (!params.no_timestamps || params.diarize) {
            printf("\n");
        }

        fflush(stdout);
    }
}

static void output_txt(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & params, std::vector<std::vector<float>> pcmf32s) {
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char * text = whisper_full_get_segment_text(ctx, i);
        std::string speaker = "";

        if (params.diarize && pcmf32s.size() == 2)
        {
            const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
            const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
            speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
        }

        fout << speaker << text << "\n";
    }
}

static void output_vtt(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & params, std::vector<std::vector<float>> pcmf32s) {
    fout << "WEBVTT\n\n";

    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char * text = whisper_full_get_segment_text(ctx, i);
        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
        std::string speaker = "";

        if (params.diarize && pcmf32s.size() == 2)
        {
            speaker = estimate_diarization_speaker(pcmf32s, t0, t1, true);
            speaker.insert(0, "<v Speaker");
            speaker.append(">");
        }

        fout << to_timestamp(t0) << " --> " << to_timestamp(t1) << "\n";
        fout << speaker << text << "\n\n";
    }
}

static void output_srt(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & params, std::vector<std::vector<float>> pcmf32s) {
    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char * text = whisper_full_get_segment_text(ctx, i);
        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
        std::string speaker = "";

        if (params.diarize && pcmf32s.size() == 2)
        {
            speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
        }

        fout << i + 1 + params.offset_n << "\n";
        fout << to_timestamp(t0, true) << " --> " << to_timestamp(t1, true) << "\n";
        fout << speaker << text << "\n\n";
    }
}

static char * escape_double_quotes_and_backslashes(const char * str) {
    if (str == NULL) {
        return NULL;
    }

    size_t escaped_length = strlen(str) + 1;

    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '"' || str[i] == '\\') {
            escaped_length++;
        }
    }

    char * escaped = (char *)calloc(escaped_length, 1); // pre-zeroed
    if (escaped == NULL) {
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '"' || str[i] == '\\') {
            escaped[pos++] = '\\';
        }
        escaped[pos++] = str[i];
    }

    // no need to set zero due to calloc() being used prior

    return escaped;
}

// double quote should be escaped by another double quote. (rfc4180)
static char * escape_double_quotes_in_csv(const char * str) {
    if (str == NULL) {
        return NULL;
    }

    size_t escaped_length = strlen(str) + 1;

    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '"') {
            escaped_length++;
        }
    }

    char *escaped = (char *)calloc(escaped_length, 1); // pre-zeroed
    if (escaped == NULL) {
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '"') {
            escaped[pos++] = '"';
        }
        escaped[pos++] = str[i];
    }

    // no need to set zero due to calloc() being used prior

    return escaped;
}

static void output_csv(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & params, std::vector<std::vector<float>> pcmf32s) {
    const int n_segments = whisper_full_n_segments(ctx);
    fout << "start,end,";
    if (params.diarize && pcmf32s.size() == 2)
    {
        fout << "speaker,";
    }
    fout << "text\n";

    for (int i = 0; i < n_segments; ++i) {
        const char * text = whisper_full_get_segment_text(ctx, i);
        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
        char * text_escaped = escape_double_quotes_in_csv(text);

        //need to multiply times returned from whisper_full_get_segment_t{0,1}() by 10 to get milliseconds.
        fout << 10 * t0 << "," << 10 * t1 << ",";
        if (params.diarize && pcmf32s.size() == 2)
        {
            fout << estimate_diarization_speaker(pcmf32s, t0, t1, true) << ",";
        }
        fout << "\"" << text_escaped << "\"\n";
    }
}

static void output_score(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & /*params*/, std::vector<std::vector<float>> /*pcmf32s*/) {
    const int n_segments = whisper_full_n_segments(ctx);
    // fprintf(stderr,"segments: %d\n",n_segments);
    for (int i = 0; i < n_segments; ++i) {
        const int n_tokens = whisper_full_n_tokens(ctx, i);
        // fprintf(stderr,"tokens: %d\n",n_tokens);
        for (int j = 0; j < n_tokens; j++) {
            auto token = whisper_full_get_token_text(ctx, i, j);
            auto probability = whisper_full_get_token_p(ctx, i, j);
            fout << token << '\t' << probability << std::endl;
            // fprintf(stderr,"token: %s %f\n",token,probability);
	    }
    }
}

static void output_json(
             struct whisper_context * ctx,
                      std::ofstream & fout,
               const whisper_params & params,
    std::vector<std::vector<float>>   pcmf32s) {
    const bool full = params.output_jsn_full;
    int indent = 0;

    auto doindent = [&]() {
        for (int i = 0; i < indent; i++) fout << "\t";
    };

    auto start_arr = [&](const char *name) {
        doindent();
        fout << "\"" << name << "\": [\n";
        indent++;
    };

    auto end_arr = [&](bool end) {
        indent--;
        doindent();
        fout << (end ? "]\n" : "],\n");
    };

    auto start_obj = [&](const char *name) {
        doindent();
        if (name) {
            fout << "\"" << name << "\": {\n";
        } else {
            fout << "{\n";
        }
        indent++;
    };

    auto end_obj = [&](bool end) {
        indent--;
        doindent();
        fout << (end ? "}\n" : "},\n");
    };

    auto start_value = [&](const char *name) {
        doindent();
        fout << "\"" << name << "\": ";
    };

    auto value_s = [&](const char *name, const char *val, bool end) {
        start_value(name);
        char * val_escaped = escape_double_quotes_and_backslashes(val);
        fout << "\"" << val_escaped << (end ? "\"\n" : "\",\n");
        free(val_escaped);
    };

    auto end_value = [&](bool end) {
        fout << (end ? "\n" : ",\n");
    };

    auto value_i = [&](const char *name, const int64_t val, bool end) {
        start_value(name);
        fout << val;
        end_value(end);
    };

    auto value_f = [&](const char *name, const float val, bool end) {
        start_value(name);
        fout << val;
        end_value(end);
    };

    auto value_b = [&](const char *name, const bool val, bool end) {
        start_value(name);
        fout << (val ? "true" : "false");
        end_value(end);
    };

    auto times_o = [&](int64_t t0, int64_t t1, bool end) {
        start_obj("timestamps");
        value_s("from", to_timestamp(t0, true).c_str(), false);
        value_s("to", to_timestamp(t1, true).c_str(), true);
        end_obj(false);
        start_obj("offsets");
        value_i("from", t0 * 10, false);
        value_i("to", t1 * 10, true);
        end_obj(end);
    };

    start_obj(nullptr);
        value_s("systeminfo", whisper_print_system_info(), false);
        start_obj("model");
            value_s("type", whisper_model_type_readable(ctx), false);
            value_b("multilingual", whisper_is_multilingual(ctx), false);
            value_i("vocab", whisper_model_n_vocab(ctx), false);
            start_obj("audio");
                value_i("ctx", whisper_model_n_audio_ctx(ctx), false);
                value_i("state", whisper_model_n_audio_state(ctx), false);
                value_i("head", whisper_model_n_audio_head(ctx), false);
                value_i("layer", whisper_model_n_audio_layer(ctx), true);
            end_obj(false);
            start_obj("text");
                value_i("ctx", whisper_model_n_text_ctx(ctx), false);
                value_i("state", whisper_model_n_text_state(ctx), false);
                value_i("head", whisper_model_n_text_head(ctx), false);
                value_i("layer", whisper_model_n_text_layer(ctx), true);
            end_obj(false);
            value_i("mels", whisper_model_n_mels(ctx), false);
            value_i("ftype", whisper_model_ftype(ctx), true);
        end_obj(false);
        start_obj("params");
            value_s("model", params.model.c_str(), false);
            value_s("language", params.language.c_str(), false);
            value_b("translate", params.translate, true);
        end_obj(false);
        start_obj("result");
            value_s("language", whisper_lang_str(whisper_full_lang_id(ctx)), true);
        end_obj(false);
        start_arr("transcription");

            const int n_segments = whisper_full_n_segments(ctx);
            for (int i = 0; i < n_segments; ++i) {
                const char * text = whisper_full_get_segment_text(ctx, i);

                const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
                const int64_t t1 = whisper_full_get_segment_t1(ctx, i);

                start_obj(nullptr);
                    times_o(t0, t1, false);
                    value_s("text", text, !params.diarize && !params.tinydiarize && !full);

                    if (full) {
                        start_arr("tokens");
                        const int n = whisper_full_n_tokens(ctx, i);
                        for (int j = 0; j < n; ++j) {
                            auto token = whisper_full_get_token_data(ctx, i, j);
                            start_obj(nullptr);
                                value_s("text", whisper_token_to_str(ctx, token.id), false);
                                if(token.t0 > -1 && token.t1 > -1) {
                                    // If we have per-token timestamps, write them out
                                    times_o(token.t0, token.t1, false);
                                }
                                value_i("id", token.id, false);
                                value_f("p", token.p, false);
                                value_f("t_dtw", token.t_dtw, true);
                            end_obj(j == (n - 1));
                        }
                        end_arr(!params.diarize && !params.tinydiarize);
                    }

                    if (params.diarize && pcmf32s.size() == 2) {
                        value_s("speaker", estimate_diarization_speaker(pcmf32s, t0, t1, true).c_str(), true);
                    }

                    if (params.tinydiarize) {
                        value_b("speaker_turn_next", whisper_full_get_segment_speaker_turn_next(ctx, i), true);
                    }
                end_obj(i == (n_segments - 1));
            }

        end_arr(true);
    end_obj(true);
}

// karaoke video generation
// outputs a bash script that uses ffmpeg to generate a video with the subtitles
// TODO: font parameter adjustments
static bool output_wts(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & params, std::vector<std::vector<float>> pcmf32s, const char * fname_inp, float t_sec, const char * fname_out) {
    static const char * font = params.font_path.c_str();

    std::ifstream fin(font);
    if (!fin.is_open()) {
        fprintf(stderr, "%s: font not found at '%s', please specify a monospace font with -fp\n", __func__, font);
        return false;
    }

    fout << "#!/bin/bash" << "\n";
    fout << "\n";

    fout << "ffmpeg -i " << fname_inp << " -f lavfi -i color=size=1200x120:duration=" << t_sec << ":rate=25:color=black -vf \"";

    for (int i = 0; i < whisper_full_n_segments(ctx); i++) {
        const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
        const int64_t t1 = whisper_full_get_segment_t1(ctx, i);

        const int n = whisper_full_n_tokens(ctx, i);

        std::vector<whisper_token_data> tokens(n);
        for (int j = 0; j < n; ++j) {
            tokens[j] = whisper_full_get_token_data(ctx, i, j);
        }

        if (i > 0) {
            fout << ",";
        }

        // background text
        fout << "drawtext=fontfile='" << font << "':fontsize=24:fontcolor=gray:x=(w-text_w)/2:y=h/2:text='':enable='between(t," << t0/100.0 << "," << t0/100.0 << ")'";

        bool is_first = true;
        std::string speaker = "";

        if (params.diarize && pcmf32s.size() == 2) {
            speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
        }

        for (int j = 0; j < n; ++j) {
            const auto & token = tokens[j];

            if (tokens[j].id >= whisper_token_eot(ctx)) {
                continue;
            }

            std::string txt_bg = "";
            std::string txt_fg = ""; // highlight token
            std::string txt_ul = ""; // underline

            if (params.diarize && pcmf32s.size() == 2) {
                txt_bg = speaker;
                txt_fg = speaker;
                txt_ul = "\\ \\ \\ \\ \\ \\ \\ \\ \\ \\ \\ ";
            }

            txt_bg.append("> ");
            txt_fg.append("> ");
            txt_ul.append("\\ \\ ");

            {
                for (int k = 0; k < n; ++k) {
                    const auto & token2 = tokens[k];

                    if (tokens[k].id >= whisper_token_eot(ctx)) {
                        continue;
                    }

                    const std::string txt = whisper_token_to_str(ctx, token2.id);

                    txt_bg += txt;

                    if (k == j) {
                        for (int l = 0; l < (int) txt.size(); ++l) {
                            txt_fg += txt[l];
                            txt_ul += "_";
                        }
                        txt_fg += "|";
                    } else {
                        for (int l = 0; l < (int) txt.size(); ++l) {
                            txt_fg += "\\ ";
                            txt_ul += "\\ ";
                        }
                    }
                }

                ::replace_all(txt_bg, "'", "\u2019");
                ::replace_all(txt_bg, "\"", "\\\"");
                ::replace_all(txt_fg, "'", "\u2019");
                ::replace_all(txt_fg, "\"", "\\\"");
            }

            if (is_first) {
                // background text
                fout << ",drawtext=fontfile='" << font << "':fontsize=24:fontcolor=gray:x=(w-text_w)/2:y=h/2:text='" << txt_bg << "':enable='between(t," << t0/100.0 << "," << t1/100.0 << ")'";
                is_first = false;
            }

            // foreground text
            fout << ",drawtext=fontfile='" << font << "':fontsize=24:fontcolor=lightgreen:x=(w-text_w)/2+8:y=h/2:text='" << txt_fg << "':enable='between(t," << token.t0/100.0 << "," << token.t1/100.0 << ")'";

            // underline
            fout << ",drawtext=fontfile='" << font << "':fontsize=24:fontcolor=lightgreen:x=(w-text_w)/2+8:y=h/2+16:text='" << txt_ul << "':enable='between(t," << token.t0/100.0 << "," << token.t1/100.0 << ")'";
        }
    }

    fout << "\" -c:v libx264 -pix_fmt yuv420p -y " << fname_inp << ".mp4" << "\n";

    fout << "\n\n";
    fout << "echo \"Your video has been saved to " << fname_inp << ".mp4\"" << "\n";
    fout << "\n";
    fout << "echo \"  ffplay " << fname_inp << ".mp4\"\n";
    fout << "\n";

    fout.close();

    fprintf(stderr, "# %s: run 'source %s' to generate karaoke video\n", __func__, fname_out);

    return true;
}

static void output_lrc(struct whisper_context * ctx, std::ofstream & fout, const whisper_params & params, std::vector<std::vector<float>> pcmf32s) {
    fout << "[by:whisper.cpp]\n";

    const int n_segments = whisper_full_n_segments(ctx);
    for (int i = 0; i < n_segments; ++i) {
        const char * text = whisper_full_get_segment_text(ctx, i);
        const int64_t t = whisper_full_get_segment_t0(ctx, i);

        int64_t msec = t * 10;
        int64_t min = msec / (1000 * 60);
        msec = msec - min * (1000 * 60);
        int64_t sec = msec / 1000;
        msec = msec - sec * 1000;

        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d.%02d", (int) min, (int) sec, (int) ( msec / 10));
        std::string timestamp_lrc = std::string(buf);
        std::string speaker = "";

        if (params.diarize && pcmf32s.size() == 2)
        {
            const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
            const int64_t t1 = whisper_full_get_segment_t1(ctx, i);
            speaker = estimate_diarization_speaker(pcmf32s, t0, t1);
        }

        fout <<  '[' << timestamp_lrc << ']' << speaker << text << "\n";
    }
}


static void cb_log_disable(enum ggml_log_level , const char * , void * ) { }

int main(int argc, char ** argv) {
#if defined(_WIN32)
    // Set the console output code page to UTF-8, while command line arguments
    // are still encoded in the system's code page. In this way, we can print
    // non-ASCII characters to the console, and access files with non-ASCII paths.
    SetConsoleOutputCP(CP_UTF8);
#endif

    whisper_params params;

    // If the only argument starts with "@", read arguments line-by-line
    // from the given file.
    std::vector<std::string> vec_args;
    if (argc == 2 && argv != nullptr && argv[1] != nullptr && argv[1][0] == '@') {
        // Save the name of the executable.
        vec_args.push_back(argv[0]);

        // Open the response file.
        char const * rspfile = argv[1] + sizeof(char);
        std::ifstream fin(rspfile);
        if (fin.is_open() == false) {
            fprintf(stderr, "error: response file '%s' not found\n", rspfile);
            return 1;
        }

        // Read the entire response file.
        std::string line;
        while (std::getline(fin, line)) {
            vec_args.push_back(line);
        }

        // Use the contents of the response file as the command-line arguments.
        argc = static_cast<int>(vec_args.size());
        argv = static_cast<char **>(alloca(argc * sizeof (char *)));
        for (int i = 0; i < argc; ++i) {
            argv[i] = const_cast<char *>(vec_args[i].c_str());
        }
    }

    if (whisper_params_parse(argc, argv, params) == false) {
        whisper_print_usage(argc, argv, params);
        return 1;
    }

    // remove non-existent files
    for (auto it = params.fname_inp.begin(); it != params.fname_inp.end();) {
        const auto fname_inp = it->c_str();

        if (*it != "-" && !is_file_exist(fname_inp)) {
            fprintf(stderr, "error: input file not found '%s'\n", fname_inp);
            it = params.fname_inp.erase(it);
            continue;
        }

        it++;
    }

    if (params.fname_inp.empty()) {
        fprintf(stderr, "error: no input files specified\n");
        whisper_print_usage(argc, argv, params);
        return 2;
    }

    if (params.language != "auto" && whisper_lang_id(params.language.c_str()) == -1) {
        fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
        whisper_print_usage(argc, argv, params);
        exit(0);
    }

    if (params.diarize && params.tinydiarize) {
        fprintf(stderr, "error: cannot use both --diarize and --tinydiarize\n");
        whisper_print_usage(argc, argv, params);
        exit(0);
    }

    if (params.no_prints) {
        whisper_log_set(cb_log_disable, NULL);
    }

    // whisper init

    struct whisper_context_params cparams = whisper_context_default_params();

    cparams.use_gpu    = params.use_gpu;
    cparams.flash_attn = params.flash_attn;

    if (!params.dtw.empty()) {
        cparams.dtw_token_timestamps = true;
        cparams.dtw_aheads_preset = WHISPER_AHEADS_NONE;

        if (params.dtw == "tiny")      cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY;
        if (params.dtw == "tiny.en")   cparams.dtw_aheads_preset = WHISPER_AHEADS_TINY_EN;
        if (params.dtw == "base")      cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE;
        if (params.dtw == "base.en")   cparams.dtw_aheads_preset = WHISPER_AHEADS_BASE_EN;
        if (params.dtw == "small")     cparams.dtw_aheads_preset = WHISPER_AHEADS_SMALL;
        if (params.dtw == "small.en")  cparams.dtw_aheads_preset = WHISPER_AHEADS_SMALL_EN;
        if (params.dtw == "medium")    cparams.dtw_aheads_preset = WHISPER_AHEADS_MEDIUM;
        if (params.dtw == "medium.en") cparams.dtw_aheads_preset = WHISPER_AHEADS_MEDIUM_EN;
        if (params.dtw == "large.v1")  cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V1;
        if (params.dtw == "large.v2")  cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V2;
        if (params.dtw == "large.v3")  cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3;
        if (params.dtw == "large.v3.turbo")  cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;

        if (cparams.dtw_aheads_preset == WHISPER_AHEADS_NONE) {
            fprintf(stderr, "error: unknown DTW preset '%s'\n", params.dtw.c_str());
            return 3;
        }
    }

    struct whisper_context * ctx = whisper_init_from_file_with_params(params.model.c_str(), cparams);

    if (ctx == nullptr) {
        fprintf(stderr, "error: failed to initialize whisper context\n");
        return 3;
    }

    // initialize openvino encoder. this has no effect on whisper.cpp builds that don't have OpenVINO configured
    whisper_ctx_init_openvino_encoder(ctx, nullptr, params.openvino_encode_device.c_str(), nullptr);

    if (!params.grammar.empty()) {
        auto & grammar = params.grammar_parsed;
        if (is_file_exist(params.grammar.c_str())) {
            // read grammar from file
            std::ifstream ifs(params.grammar.c_str());
            const std::string txt = std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            grammar = grammar_parser::parse(txt.c_str());
        } else {
            // read grammar from string
            grammar = grammar_parser::parse(params.grammar.c_str());
        }

        // will be empty (default) if there are parse errors
        if (grammar.rules.empty()) {
            fprintf(stderr, "error: failed to parse grammar \"%s\"\n", params.grammar.c_str());
            return 4;
        } else {
            fprintf(stderr, "%s: grammar:\n", __func__);
            grammar_parser::print_grammar(stderr, grammar);
            fprintf(stderr, "\n");
        }
    }

    // Register signal handler for Ctrl+C
    signal(SIGINT, signalHandler);

    // Initialize PortAudio
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    // Display available audio devices
    int numDevices = Pa_GetDeviceCount();
    printf("Available audio input devices:\n");
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(i);
        if (deviceInfo->maxInputChannels > 0) {
            printf("  [%d] %s\n", i, deviceInfo->name);
        }
    }

    // Select input device
    int deviceIndex = params.device_index;
    if (deviceIndex == -1) {
        deviceIndex = Pa_GetDefaultInputDevice();
        printf("Using default input device: [%d] %s\n", 
               deviceIndex, 
               Pa_GetDeviceInfo(deviceIndex)->name);
    } else {
        if (deviceIndex >= 0 && deviceIndex < numDevices) {
            printf("Using selected input device: [%d] %s\n", 
                   deviceIndex, 
                   Pa_GetDeviceInfo(deviceIndex)->name);
        } else {
            fprintf(stderr, "Invalid device index %d. Using default.\n", deviceIndex);
            deviceIndex = Pa_GetDefaultInputDevice();
        }
    }

    // Prepare stream parameters
    PaStreamParameters inputParameters;
    memset(&inputParameters, 0, sizeof(inputParameters));
    inputParameters.device = deviceIndex;
    inputParameters.channelCount = 1;  // Mono input
    inputParameters.sampleFormat = paFloat32;
    inputParameters.suggestedLatency = Pa_GetDeviceInfo(deviceIndex)->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    // Create user data for the audio callback
    std::vector<float> recorded_data;
    paUserData userData;
    userData.recorded_data = &recorded_data;
    userData.ctx = ctx;
    userData.params = &params;
    userData.processing = false;
    userData.buffer_size = params.sample_rate * 3; // 3 seconds buffer
    userData.buffer.resize(userData.buffer_size);
    userData.buffer_index = 0;

    // Open the audio stream
    PaStream *stream;
    err = Pa_OpenStream(&stream,
                        &inputParameters,
                        NULL,  // No output
                        params.sample_rate,
                        params.frames_per_buffer,
                        paClipOff,
                        recordCallback,
                        &userData);
    
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        whisper_free(ctx);
        Pa_Terminate();
        return 1;
    }

    // Start the audio stream
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        whisper_free(ctx);
        Pa_Terminate();
        return 1;
    }

    printf("\n");
    printf("Recording started! Speak into the microphone.\n");
    printf("Press Ctrl+C to stop the recording.\n");
    printf("\n");

    // Calculate when to stop if record_duration is set
    double stopTime = 0;
    if (params.record_duration > 0) {
        stopTime = Pa_GetStreamTime(stream) + params.record_duration;
    }

    // Main loop - just wait until interrupted or duration reached
    while (is_running) {
        Pa_Sleep(100); // Sleep to reduce CPU usage
        
        // Check if we've reached the specified duration
        if (params.record_duration > 0 && Pa_GetStreamTime(stream) >= stopTime) {
            printf("Recording duration reached.\n");
            is_running = false;
        }
    }

    // Stop and close the stream
    err = Pa_StopStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    }

    err = Pa_CloseStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    }

    // Clean up
    Pa_Terminate();
    whisper_free(ctx);

    printf("Recording stopped. Processed %zu samples.\n", recorded_data.size());
    return 0;
}
