#pragma once
#include <filesystem>
#include <optional>
#include <vector>

namespace gao {

// Whether "Automatic Tuning" of the NVIDIA app is switched on for a graphics
// card that is in this PC. Switched on, a service of the driver loads the
// card and changes its clocks when it sees fit, and starts again by itself
// after it was stopped.
//
// Read from the driver's own settings database
// (%ProgramData%\NVIDIA Corporation\nvtopps\nvtopps.db3, table
// ocScannerSettings), which is where that service keeps the switch: with the
// field set it took the scan up again after every restart, with the field
// cleared it did not (hardware check 89). Nothing documents this file, so
// everything about it may change with a driver: nullopt when the file, the
// table or a column is not there, or when Windows' SQLite library is not.
// The file is opened read-only and without taking a lock on it.
std::optional<bool> nvidia_auto_tuning();

// The same from a database file of the caller's choosing, for the cards with
// these PCI device ids (0x2786 is an RTX 4070). A row of a card that is no
// longer in the PC does not count: nobody could switch it off any more.
std::optional<bool> nvidia_auto_tuning_in(const std::filesystem::path& database, const std::vector<unsigned>& device_ids);

}
