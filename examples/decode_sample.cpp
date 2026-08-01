// decode_sample.cpp — 最小限の使用例
//   32byte (250bit) の QZSS L1S MT43/44 メッセージを16進文字列（64桁）で
//   コマンドライン引数に渡すと、共通部＋カテゴリ別詳細のサマリを表示する。
//
//   使い方:
//     decode_sample <64桁の16進文字列>
//
//   例（ダミー値・実際のデコード結果は保証しない）:
//     decode_sample 0000000000000000000000000000000000000000000000000000000000000000
#include "dcr_report.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool parseHex(const char* hex, uint8_t out[32]) {
    if (std::strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (std::sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <64-hex-digit L1S message>\n", argv[0]);
        return 1;
    }

    uint8_t msg[32];
    if (!parseHex(argv[1], msg)) {
        std::fprintf(stderr, "error: expected exactly 64 hex digits (32 bytes)\n");
        return 1;
    }

    DcrReport r;
    if (!dcr_decode(msg, r)) {
        std::printf("decode failed (CRC24 / PAB / MT mismatch)\n");
        return 1;
    }

    std::printf("mt=%u valid=%d jmaValid=%d\n", r.mt, r.valid, r.jmaValid);
    if (r.jmaValid) {
        std::printf("Rc=%u (%s)  Dc=%u (%s)  It=%u (%s)\n",
                    r.classification, dcr_classification_name(r.classification),
                    r.category, dcr_category_name(r.category),
                    r.infoType, dcr_info_type_name(r.infoType));
        std::printf("At (UTC) = %02u/%02u %02u:%02u\n", r.month, r.day, r.hour, r.minute);

        char buf[256];
        dcr_summary(r, buf, sizeof(buf));
        std::printf("summary: %s\n", buf);
    }
    return 0;
}
