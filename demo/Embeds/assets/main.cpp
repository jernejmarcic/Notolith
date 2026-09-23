// A tiny C++ file embedded whole with ![[main.cpp]] - source-file embed test.
#include <iostream>
#include <string>
#include <vector>

struct Note {
    std::string title;
    std::vector<std::string> tags;
};

int main()
{
    Note note{"Embedded Source File", {"demo", "embed"}};
    std::cout << note.title << " (" << note.tags.size() << " tags)\n";
    return 0;
}
