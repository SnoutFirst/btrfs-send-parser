#pragma once

//Command line front end for the library: reads one or more send streams and prints what is in them.
//The argument handling lives here so that main.cpp stays a three line process entry point.

#include <iosfwd>
#include <string>
#include <vector>

#include "btrfs/send.hpp"

namespace dump {

enum class Format
{
    Text,      //one line per operation, optionally one line per attribute
    Json,      //one JSON document per input
    JsonLines, //one JSON object per operation, so a huge stream can be consumed line by line
};

struct Options
{
    Format format = Format::Text;
    bool show_help = false;
    bool show_version = false;
    bool print_attributes = false;      //text output: one additional line per attribute
    bool print_stats = false;           //summary block at the end of every input
    bool print_changed_extents = false; //UPDATE_EXTENT table instead of the operation listing
    btrfs::send::Parser::Options parser;
};

//Fills options and inputs from argv. An empty input list means standard input, and a single "-" means the
//same. Returns false and sets error when the arguments cannot be used; the caller prints the usage text.
bool parse_arguments(int argc, char** argv, Options& options, std::vector<std::string>& inputs, std::string& error);

void print_usage(std::ostream& out, const char* program);

//Parses every input in order. Returns 0 when all of them parsed cleanly, 1 when a stream was malformed or
//could not be read, and 2 when the arguments were wrong (which the caller reports before calling this).
int run(const Options& options, const std::vector<std::string>& inputs, std::ostream& out, std::ostream& err);

}
