# RefBitClockCache: A Thread-Safe Cache for ESP-IDF

This repository contains a robust, thread-safe cache implementation in C, specifically designed for use with the **ESP-IDF** framework. The cache employs a hash table for rapid key-based lookups and an advanced **Clock with Reference Bit** eviction policy, making it highly efficient for managing memory in resource-constrained, multi-threaded environments.

## Features

- **Thread-Safe:** Uses a FreeRTOS mutex (`SemaphoreHandle_t`) to protect concurrent access to the cache, ensuring data integrity in multi-tasking applications.
- **Efficient Eviction:** The **Clock with Reference Bit algorithm** efficiently selects a victim for eviction. It gives a second chance to frequently used items, preventing them from being removed prematurely. This policy is an improvement over a simple FIFO (First-In, First-Out) or Clock algorithm.
- **Dynamic Hashing:** The internal hash table uses **linear probing** to handle collisions and automatically **rehashes** to a larger prime size as the number of entries grows, maintaining lookup performance.
- **Reference Counting:** Includes a `refcount` mechanism on `CacheValue` objects. This ensures that data is not freed while it's still being held or used by a thread, preventing use-after-free bugs. The data is only truly freed when its reference count drops to zero.
- **Generic Data Storage:** The cache is designed to be flexible. It can store any type of data (`void*`) and uses a user-provided `value_free` function for proper deallocation.
- **Logging:** Integrates with the ESP-IDF logging system (`ESP_LOGI`, `ESP_LOGE`, etc.) to provide detailed information on cache hits, misses, evictions, and potential issues.

## How It Works

### Eviction Policy (Clock with Reference Bit)

The core of this cache is its eviction algorithm. A "clock hand" moves sequentially through the cache entries. For each entry, it checks two conditions:

1. **Reference Count (`refcount`):** If `refcount` is greater than 0, it means a thread is currently holding a reference to the data. This item cannot be a victim.
2. **Reference Bit (`ref_bit`):** This bit is set to `1` every time an entry is accessed. The clock hand checks this bit.
   - If `ref_bit` is `1`, the item has been recently used. The clock hand resets the bit to `0` and moves on, giving the item a "second chance."
   - If `ref_bit` is `0`, the item has not been used since the clock hand last passed it. It is considered a suitable **victim** for eviction.

This process continues until an appropriate victim is found or a full pass has been made.

### Hash Table

The cache uses a hash table with open addressing (linear probing) to map a string key to an index in the cache array. This provides an average-case O(1) complexity for lookups, insertions, and deletions.

- **State Management:** Each hash entry has a `state` (`STATE_EMPTY`, `STATE_OCCUPIED`, `STATE_TOMBSTONE`) to manage insertions and deletions effectively. `TOMBSTONE` entries are used to prevent disruption of lookup chains after a deletion.
- **Rehashing:** To prevent performance degradation from a high load factor, the hash table automatically doubles in size and rehashes all existing entries when it becomes more than 70% full.

## Project Structure

| File                  | Description |
|-----------------------|-------------|
| `refbit_clock_cache.h` | Header file containing structure definitions, function prototypes, and macros for the cache implementation. |
| `refbit_clock_cache.c` | The source file implementing the cache logic, including hashing, eviction, and thread-safety mechanisms. |
| `main.c`              | The test application demonstrating cache usage in a multi-threaded scenario, including creation, access, release, and destruction. It includes the cache header for integration. |

## Usage

This code is a complete ESP-IDF project example. To build and flash it to an ESP32 board, follow these steps:

1. Make sure you have the ESP-IDF environment set up.
2. Clone this repository.
3. Navigate to the project directory.
4. Update your `CMakeLists.txt` to include the source files (e.g., `idf_component_register(SRCS "refbit_clock_cache.c" "main.c" INCLUDE_DIRS ".")`).
5. Run the following command, replacing `COM5` with your device's port:

   ```sh
   idf.py -p COM5 -b 115200 flash monitor
   ```
### Integrating into Your Project

- Include `refbit_clock_cache.h` in any source file where you want to use the cache.
- Call `createCache()` to initialize with a desired size and a custom free function (or use the default `freeValue()`).
- Use `accessCache()` to get or insert data (returns a `CacheValue*` with refcount incremented).
- Always call `releaseValue()` when done with the `CacheValue*` to decrement refcount.
- Call `freeCache()` to clean up the entire cache.

### Example Log Output

The log output provides real-time insight into the cache's operation, showing hits, misses, and the state of the cache.
```
I (12345) RefBitClockCache: Cache hit → key: A in line 0 ref=2, bit=1
I (12346) RefBitClockCache: Cache state (hand=1): [0: A, ref=2, bit=1] [1: B, ref=1, bit=1]
I (12347) RefBitClockCache: Cache miss → stored key: C in line 2 ref=1, bit=1 (victim was 2)
I (12348) RefBitClockCache: Cache state (hand=3): [0: A, ref=2, bit=1] [1: B, ref=1, bit=1] [2: C, ref=1, bit=1]
```
