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

// One unfinished candidate of an earlier run.
struct Freeze {
    std::string description;      // "core +C", "mem +M", both, or "stock clocks"
    // False when no clock of the entry is above stock: the machine froze, but
    // there is no setting a later run could stay below.
    bool caps_anything = false;
};

// Write-ahead log of every clock candidate. begin() is written (and, in
// production, flushed to disk) before the candidate is applied; complete()
// after its probe. A begin with no complete therefore marks the candidate
// that froze the machine, and it becomes a ceiling for every later run. An
// offset of 0 is never a ceiling: that would end the search of that clock.
// Pure: the file lives in hw/app_files, reached through `append`.
class Journal {
public:
    using Append = std::function<bool(const std::string&)>;

    Journal(const std::vector<std::string>& existing_lines, Append append);

    const Ceilings& ceilings() const { return ceilings_; }
    // Human-readable description of each unfinished candidate, in file order:
    // its clocks, or "stock clocks" when none of them is above stock.
    const std::vector<std::string>& freezes() const { return freezes_; }
    // The same candidates, each with whether it lowered a ceiling.
    const std::vector<Freeze>& freeze_entries() const { return freeze_entries_; }
    int next_id() const { return next_id_; }
    // The entry begin() opened and complete() has not closed; -1 when none.
    int open_id() const { return open_id_; }

    // Returns the entry id, or -1 when the line could not be written -- in
    // which case the caller must not touch the hardware.
    int begin(std::optional<int> core, std::optional<int> mem);
    bool complete(int id, const std::string& verdict);

private:
    bool write(const std::string& line);

    Append append_;
    // The existing file ended in an unparseable line -- most likely one torn
    // by a power cut, without its newline. The next write starts with '\n'
    // so it is not glued onto that fragment.
    bool torn_tail_ = false;
    Ceilings ceilings_;
    std::vector<std::string> freezes_;
    std::vector<Freeze> freeze_entries_;
    int next_id_ = 1;
    int open_id_ = -1;
};

}
