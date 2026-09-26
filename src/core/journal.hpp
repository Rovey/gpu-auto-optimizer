#pragma once
#include <climits>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gao {

// Exclusive upper bounds the search must stay below. INT_MAX = no limit.
struct Ceilings {
    int core_mhz = INT_MAX;
    int mem_mhz = INT_MAX;
};

// Write-ahead log of every clock candidate. begin() is written (and, in
// production, flushed to disk) before the candidate is applied; complete()
// after its probe. A begin with no complete therefore marks the candidate
// that froze the machine, and it becomes a ceiling for every later run.
// Pure: the file lives in hw/journal_file, reached through `append`.
class Journal {
public:
    using Append = std::function<bool(const std::string&)>;

    Journal(const std::vector<std::string>& existing_lines, Append append);

    const Ceilings& ceilings() const { return ceilings_; }
    // Human-readable description of each unfinished candidate, in file order.
    const std::vector<std::string>& freezes() const { return freezes_; }
    int next_id() const { return next_id_; }

    // Returns the entry id, or -1 when the line could not be written -- in
    // which case the caller must not touch the hardware.
    int begin(std::optional<int> core, std::optional<int> mem);
    bool complete(int id, const std::string& verdict);

private:
    Append append_;
    Ceilings ceilings_;
    std::vector<std::string> freezes_;
    int next_id_ = 1;
};

}
