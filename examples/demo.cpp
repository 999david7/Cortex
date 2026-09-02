// A tiny playground file for trying out the AI features.
//
// Put the caret at the end of a line and press Ctrl+Space to ask your local
// model for a completion. Select a block and use Explain / Improve / Fix.

#include <cstdio>
#include <string>
#include <vector>

// Returns the number of vowels in given text.
int count_vowels(const std::string& text) {
    int count = 0;
    for (char c : text) {
        switch (c) {
            case 'a': case 'e': case 'i': case 'o': case 'u':
                ++count;
                break;
            default:
                break;
        }
    }
    return count;
}

// Splits a string on a delimiter.
std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

int main() {
    const std::string line = "the quick brown fox jumps over the lazy dog";
    std::printf("vowels: %d\n", count_vowels(line));

    for (const std::string& word : split(line, ' ')) {
        std::printf("%s\n", word.c_str());
    }
    return 0;
}
