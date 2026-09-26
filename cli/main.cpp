#include "dump.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    dump::Options options;
    std::vector<std::string> inputs;
    std::string error;

    if (!dump::parse_arguments(argc, argv, options, inputs, error))
    {
        std::cerr << "btrfs-send-dump: " << error << "\n\n";
        dump::print_usage(std::cerr, "btrfs-send-dump");
        return 2;
    }
    if (options.show_help)
    {
        dump::print_usage(std::cout, "btrfs-send-dump");
        return 0;
    }

    return dump::run(options, inputs, std::cout, std::cerr);
}
