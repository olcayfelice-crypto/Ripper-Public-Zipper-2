#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>

#include <zstd.h>

namespace fs = std::filesystem;

static constexpr char MAGIC[] = "RPZ3";
static constexpr uint32_t VERSION = 3;

enum Method : uint8_t {
    STORE = 0,
    ZSTD  = 1
};

struct Entry {
    std::string path;
    uint8_t method;
    uint64_t original_size;
    uint64_t compressed_size;
};

static void write8(std::ofstream& out, uint8_t v) {
    out.write(reinterpret_cast<const char*>(&v), 1);
}

static void write32(std::ofstream& out, uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), 4);
}

static void write64(std::ofstream& out, uint64_t v) {
    out.write(reinterpret_cast<const char*>(&v), 8);
}

static uint8_t read8(std::ifstream& in) {
    uint8_t v{};
    in.read(reinterpret_cast<char*>(&v), 1);
    return v;
}

static uint32_t read32(std::ifstream& in) {
    uint32_t v{};
    in.read(reinterpret_cast<char*>(&v), 4);
    return v;
}

static uint64_t read64(std::ifstream& in) {
    uint64_t v{};
    in.read(reinterpret_cast<char*>(&v), 8);
    return v;
}

static void write_string(
    std::ofstream& out,
    const std::string& s
) {
    write32(out, static_cast<uint32_t>(s.size()));

    if (!s.empty()) {
        out.write(
            s.data(),
            static_cast<std::streamsize>(s.size())
        );
    }
}

static std::string read_string(std::ifstream& in) {
    uint32_t size = read32(in);

    std::string result(size, '\0');

    if (size) {
        in.read(
            result.data(),
            static_cast<std::streamsize>(size)
        );
    }

    return result;
}

static std::vector<uint8_t> read_file(
    const fs::path& path
) {
    std::ifstream in(
        path,
        std::ios::binary
    );

    if (!in)
        throw std::runtime_error(
            "Dosya acilamadi: " +
            path.string()
        );

    in.seekg(0, std::ios::end);

    std::streamoff size = in.tellg();

    in.seekg(0);

    std::vector<uint8_t> data(
        static_cast<size_t>(size)
    );

    if (size > 0) {
        in.read(
            reinterpret_cast<char*>(data.data()),
            size
        );
    }

    return data;
}

static bool store_file(
    const fs::path& path
) {
    std::string ext =
        path.extension().string();

    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        [](unsigned char c) {
            return static_cast<char>(
                std::tolower(c)
            );
        }
    );

    return
        ext == ".cpp" ||
        ext == ".java";
}

static std::vector<uint8_t> zstd_compress(
    const std::vector<uint8_t>& input
) {
    if (input.empty())
        return {};

    size_t bound =
        ZSTD_compressBound(input.size());

    std::vector<uint8_t> output(bound);

    /*
        22 = Zstandard'ın yüksek
        sıkıştırma seviyelerinden biri.
    */

    size_t result = ZSTD_compress(
        output.data(),
        output.size(),
        input.data(),
        input.size(),
        22
    );

    if (ZSTD_isError(result)) {
        throw std::runtime_error(
            ZSTD_getErrorName(result)
        );
    }

    output.resize(result);

    return output;
}

static std::vector<uint8_t> zstd_decompress(
    const std::vector<uint8_t>& input,
    uint64_t original_size
) {
    std::vector<uint8_t> output(
        static_cast<size_t>(original_size)
    );

    if (original_size == 0)
        return output;

    size_t result = ZSTD_decompress(
        output.data(),
        output.size(),
        input.data(),
        input.size()
    );

    if (ZSTD_isError(result)) {
        throw std::runtime_error(
            ZSTD_getErrorName(result)
        );
    }

    if (result != original_size) {
        throw std::runtime_error(
            "Boyut dogrulamasi basarisiz."
        );
    }

    return output;
}

static void pack(
    const fs::path& source,
    const fs::path& output
) {
    if (!fs::exists(source)) {
        throw std::runtime_error(
            "Klasor bulunamadi: " +
            source.string()
        );
    }

    std::vector<Entry> entries;
    std::vector<std::vector<uint8_t>> blocks;

    uint64_t original_total = 0;
    uint64_t compressed_total = 0;

    for (
        const auto& item :
        fs::recursive_directory_iterator(source)
    ) {
        if (!item.is_regular_file())
            continue;

        auto original =
            read_file(item.path());

        Entry entry;

        entry.path =
            fs::relative(
                item.path(),
                source
            ).generic_string();

        entry.original_size =
            original.size();

        std::vector<uint8_t> compressed;

        /*
            Kullanıcının kuralı:
            .cpp ve .java sıkıştırılmaz.
        */

        if (store_file(item.path())) {

            entry.method = STORE;
            compressed = std::move(original);

        } else {

            auto candidate =
                zstd_compress(original);

            /*
                Sıkıştırılmış veri daha büyükse
                STORE kullan.
            */

            if (
                candidate.size() <
                original.size()
            ) {
                entry.method = ZSTD;
                compressed =
                    std::move(candidate);
            } else {
                entry.method = STORE;
                compressed =
                    std::move(original);
            }
        }

        entry.compressed_size =
            compressed.size();

        original_total +=
            entry.original_size;

        compressed_total +=
            entry.compressed_size;

        entries.push_back(entry);
        blocks.push_back(std::move(compressed));
    }

    std::ofstream out(
        output,
        std::ios::binary |
        std::ios::trunc
    );

    if (!out) {
        throw std::runtime_error(
            "RPZ dosyasi olusturulamadi."
        );
    }

    out.write(MAGIC, 4);

    write32(out, VERSION);

    write32(
        out,
        static_cast<uint32_t>(
            entries.size()
        )
    );

    /*
        Index
    */

    for (const auto& entry : entries) {

        write_string(
            out,
            entry.path
        );

        write8(
            out,
            entry.method
        );

        write64(
            out,
            entry.original_size
        );

        write64(
            out,
            entry.compressed_size
        );
    }

    /*
        Data
    */

    for (const auto& block : blocks) {

        if (!block.empty()) {
            out.write(
                reinterpret_cast<const char*>(
                    block.data()
                ),
                static_cast<std::streamsize>(
                    block.size()
                )
            );
        }
    }

    std::cout
        << "RPZ olusturuldu!\n\n";

    std::cout
        << "Orijinal : "
        << original_total
        << " bytes\n";

    std::cout
        << "Veri      : "
        << compressed_total
        << " bytes\n\n";

    for (const auto& entry : entries) {

        std::cout
            << (entry.method == STORE
                ? "[STORE]    "
                : "[ZSTD-22]  ")
            << entry.path
            << "  "
            << entry.original_size
            << " -> "
            << entry.compressed_size
            << "\n";
    }
}

static std::vector<Entry> read_index(
    std::ifstream& in
) {
    char magic[4];

    in.read(magic, 4);

    if (
        std::string(magic, 4) !=
        "RPZ2"
    ) {
        throw std::runtime_error(
            "Bu RPZ degil."
        );
    }

    uint32_t version =
        read32(in);

    if (version != VERSION) {
        throw std::runtime_error(
            "Desteklenmeyen RPZ surumu."
        );
    }

    uint32_t count =
        read32(in);

    std::vector<Entry> entries;

    for (uint32_t i = 0; i < count; ++i) {

        Entry entry;

        entry.path =
            read_string(in);

        entry.method =
            read8(in);

        entry.original_size =
            read64(in);

        entry.compressed_size =
            read64(in);

        entries.push_back(
            std::move(entry)
        );
    }

    return entries;
}

static void list_archive(
    const fs::path& archive
) {
    std::ifstream in(
        archive,
        std::ios::binary
    );

    if (!in)
        throw std::runtime_error(
            "Arsiv acilamadi."
        );

    auto entries =
        read_index(in);

    std::cout
        << "RPZ v3\n\n";

    for (const auto& entry : entries) {

        std::cout
            << entry.path
            << " | "
            << (
                entry.method == STORE
                ? "STORE"
                : "ZSTD-22"
            )
            << " | "
            << entry.original_size
            << " -> "
            << entry.compressed_size
            << "\n";
    }
}

static void extract(
    const fs::path& archive,
    const fs::path& destination
) {
    std::ifstream in(
        archive,
        std::ios::binary
    );

    if (!in)
        throw std::runtime_error(
            "Arsiv acilamadi."
        );

    auto entries =
        read_index(in);

    for (const auto& entry : entries) {

        std::vector<uint8_t> block(
            static_cast<size_t>(
                entry.compressed_size
            )
        );

        if (!block.empty()) {
            in.read(
                reinterpret_cast<char*>(
                    block.data()
                ),
                static_cast<std::streamsize>(
                    block.size()
                )
            );
        }

        std::vector<uint8_t> original;

        if (entry.method == STORE) {

            original =
                std::move(block);

        } else {

            original =
                zstd_decompress(
                    block,
                    entry.original_size
                );
        }

        fs::path target =
            destination /
            fs::path(entry.path);

        fs::create_directories(
            target.parent_path()
        );

        std::ofstream out(
            target,
            std::ios::binary |
            std::ios::trunc
        );

        if (!out)
            throw std::runtime_error(
                "Dosya yazilamadi: " +
                target.string()
            );

        if (!original.empty()) {
            out.write(
                reinterpret_cast<const char*>(
                    original.data()
                ),
                static_cast<std::streamsize>(
                    original.size()
                )
            );
        }

        std::cout
            << "OK "
            << entry.path
            << "\n";
    }
}

static void usage() {
    std::cout <<
R"(RPZ - Ripper Public Zipper

Usage:

  rpz pack <folder> <output.rpz>
  rpz list <archive.rpz>
  rpz extract <archive.rpz> <folder>
)";
}

int main(
    int argc,
    char* argv[]
) {
    try {

        if (argc < 2) {
            usage();
            return 1;
        }

        std::string command =
            argv[1];

        if (command == "pack") {

            if (argc != 4) {
                usage();
                return 1;
            }

            pack(
                argv[2],
                argv[3]
            );

            return 0;
        }

        if (command == "list") {

            if (argc != 3) {
                usage();
                return 1;
            }

            list_archive(argv[2]);

            return 0;
        }

        if (command == "extract") {

            if (argc != 4) {
                usage();
                return 1;
            }

            extract(
                argv[2],
                argv[3]
            );

            return 0;
        }

        usage();
        return 1;

    } catch (
        const std::exception& e
    ) {

        std::cerr
            << "RPZ ERROR: "
            << e.what()
            << "\n";

        return 1;
    }
}