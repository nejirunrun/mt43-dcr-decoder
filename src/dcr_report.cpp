// dcr_report.cpp — QZSS 災危通報デコーダ実装（純関数・状態ゼロ）
// =============================================================================
// 【役割】
//   dcr_report.h の実装本体。値テーブル（震度・地域・府県予報区…）もここに置く。
//   「32byte → 値」だけを担当し、受信・保管・表示レイヤは一切知らない。
//   標準ライブラリ（<cstdint> 等）のみに依存し、Arduino/ESP32環境に依存しない。
//
// 【非自明な定数の根拠】
//   ・CRC24 多項式 0x1864CFB / 対象 226bit … IS-QZSS-DCR §4.1.1.3
//   ・PAB 0x53/0x9A/0xC6 … IS-QZSS-L1S §4.1.2.2（6秒周期で巡回）
//   ・EEW の仮定震源要素 depth==10 && mag==10 … JMA の PLUM 法等で震源未確定のとき
//     深さ 10km・M1.0 を埋める運用（額面どおり表示すると誤解を招く）
//
// 【既知の限界】
//   ・MT44（DCX＝他機関）は共通部からして別フォーマット。本モジュールは
//     valid/mt までしか埋めない（jmaValid=false）。
//   ・座標を積むのは震源(2)・台風(12)のみ。EEW(1) は地域コードで
//     「どこが揺れるか」を伝える形式で、構造的に座標を持たない。
// =============================================================================
#include "dcr_report.h"
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────────
// 低レベル：ビット読み・CRC24・PAB
//   msg[] は MSB-first パック済み 32byte（最終 byte は上位 2bit のみ有効＝250bit）。
// ─────────────────────────────────────────────────────────────
uint32_t dcr_bits(const uint8_t* msg, int bitOffset, int bitLen) {
    uint32_t value = 0;
    for (int i = 0; i < bitLen; i++) {
        int bitPos    = bitOffset + i;
        int byteIdx   = bitPos >> 3;
        int bitInByte = 7 - (bitPos & 7);
        value = (value << 1) | ((msg[byteIdx] >> bitInByte) & 1);
    }
    return value;
}

bool dcr_pab_valid(uint8_t pab) {
    return pab == 0x53 || pab == 0x9A || pab == 0xC6;
}

// CRC24（IS-QZSS-DCR §4.1.1.3）。対象は PAB(8)+MT(6)+DATA FIELD(212) = 226bit。
//   ★用途は**受信の破損検出のみ**。重複判定には使わないこと（同一内容でも値が変わる
//     ことが実測で確認済み＝dcr_report.h の設計判断②）。重複判定は dcr_content_key()。
static uint32_t crc24(const uint8_t* msg) {
    uint8_t data[29];
    memcpy(data, msg, 28);
    data[28] = msg[28] & 0xC0;

    uint32_t crc = 0;
    int remaining = 226;
    for (int b = 0; b < 29 && remaining > 0; b++) {
        crc ^= ((uint32_t)data[b] << 16);
        for (int k = 0; k < 8 && remaining > 0; k++) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFBUL;
            remaining--;
        }
    }
    return crc & 0xFFFFFF;
}

// ─────────────────────────────────────────────────────────────
// 共通部デコード
// ─────────────────────────────────────────────────────────────
bool dcr_decode(const uint8_t msg[32], DcrReport& out) {
    memset(&out, 0, sizeof(out));

    uint8_t pab = dcr_bits(msg, 0, 8);
    uint8_t mt  = dcr_bits(msg, 8, 6);
    if (!dcr_pab_valid(pab))      return false;
    if (mt != 43 && mt != 44)     return false;   // 災危通報以外（MT47〜51/63 等）
    if (dcr_bits(msg, 226, 24) != crc24(msg)) return false;   // 破損受信

    out.valid = true;
    out.mt    = mt;
    memcpy(out.raw, msg, 32);

    // MT44（DCX）は共通部から別レイアウト。ここで当てはめると無意味な値になる
    // （実測で at_mo=0 等の不正値が出て発覚・2026-07-27）ので触らない。
    if (mt == 43) {
        out.jmaValid       = true;
        out.classification = dcr_bits(msg, 14, 3);
        out.category       = dcr_bits(msg, 17, 4);
        out.month          = dcr_bits(msg, 21, 4);
        out.day            = dcr_bits(msg, 25, 5);
        out.hour           = dcr_bits(msg, 30, 5);
        out.minute         = dcr_bits(msg, 35, 6);
        out.infoType       = dcr_bits(msg, 41, 2);
    }
    // 詳細デコーダを踏まえた内容キーを最後に作る（共通部が埋まってから呼ぶ必要がある）
    out.contentKey = dcr_content_key(out);
    return true;
}

// ─────────────────────────────────────────────────────────────
// UTC → JST（+9時間）
//
//   ★電文の時刻は UTC（2026-07-28 実測で確定）。受信した全通報が例外なく日本時間から
//     9時間遅れており、14:06 の通報を 23:06 JST に受信して発覚した。
//     当初コメントで「JST」と誤って断定していたため、表示がそのまま9時間ずれていた。
//
//   月をまたぐ加算が必要（例: 3月31日 23:30 UTC → 4月1日 08:30 JST）。
//   電文に年が無いので閏年を判定できず、2月は平年（28日）として扱う。
// ─────────────────────────────────────────────────────────────
void dcr_to_jst(uint8_t* month, uint8_t* day, uint8_t* hour, uint8_t* minute) {
    if (!hour) return;
    static const uint8_t kDim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    int h = *hour + 9;
    if (h < 24) { *hour = (uint8_t)h; return; }   // 日をまたがない
    *hour = (uint8_t)(h - 24);
    if (!day) return;

    int d = *day + 1;
    int mo = month ? *month : 0;
    int dim = (mo >= 1 && mo <= 12) ? kDim[mo - 1] : 31;
    if (d > dim) {
        d = 1;
        if (month) *month = (uint8_t)((mo % 12) + 1);
    }
    *day = (uint8_t)d;
    (void)minute;   // 分は時差の影響を受けない（+9:00 ちょうど）
}

// ─────────────────────────────────────────────────────────────
// 内容キー — 再送の同一判定
//
//   FNV-1a（32bit）で「意味の取れたフィールド」だけを順に混ぜる。生バイトは混ぜない。
//   ★なぜ生バイト/CRC ではないのか:
//     MT43 には確定した区切り・長さ・順番が無く、観測側からその構造を知る術がない。
//     同じ通報がカルーセルしているように「見える」だけで、ビット列が同一である保証は
//     どこにも無い（実測でも、同一内容の再送が別のビット列になる例を確認済み。
//     詳細は docs/DECODER_NOTES.md ①）。デコードして意味が確定したものだけを鍵にする。
//
//   MT44（DCX）は詳細デコーダが無く共通部も別フォーマットなので、キーは作れない
//   （0 を返す＝呼び出し側で「重複判定不能」として扱う）。
// ─────────────────────────────────────────────────────────────
static inline void fnvMix(uint32_t& h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (uint8_t)(v >> (i * 8));
        h *= 16777619u;
    }
}

uint32_t dcr_content_key(const DcrReport& r) {
    if (!r.valid || !r.jmaValid) return 0;

    uint32_t h = 2166136261u;
    fnvMix(h, r.mt);
    fnvMix(h, r.category);
    fnvMix(h, r.classification);
    fnvMix(h, r.infoType);
    // 報告時刻（月日時分）。同じ災害でも続報は時刻が進むので別通報として扱われる＝正しい。
    fnvMix(h, ((uint32_t)r.month << 24) | ((uint32_t)r.day << 16) |
              ((uint32_t)r.hour << 8) | r.minute);

    // カテゴリ別詳細。★全14カテゴリの詳細デコーダの結果をキーに混ぜる。詳細を混ぜない
    //   カテゴリは、報告時刻が同じで内容だけが違う複数メッセージ（例: 台風の実況と
    //   予報レグ）を誤って同一通報とみなしてしまう（詳細は docs/DECODER_NOTES.md ⑥）。
    if (r.category == DCR_CAT_EEW) {
        DcrEew e;
        if (dcr_eew(r, e)) {
            fnvMix(h, ((uint32_t)e.otDay << 16) | ((uint32_t)e.otHour << 8) | e.otMin);
            fnvMix(h, ((uint32_t)e.depthRaw << 16) | ((uint32_t)e.magRaw << 8) | e.epicenterCode);
            fnvMix(h, ((uint32_t)e.intensityLower << 8) | e.intensityUpper);
            fnvMix(h, ((uint32_t)e.lgLower << 8) | e.lgUpper);
            for (int i = 0; i < 3; i++) fnvMix(h, e.prevention[i]);
            for (int i = 0; i < 10; i++) fnvMix(h, e.regionBits[i]);
        }
    } else if (r.category == DCR_CAT_HYPOCENTER) {
        DcrHypocenter p;
        if (dcr_hypocenter(r, p)) {
            fnvMix(h, ((uint32_t)p.otDay << 16) | ((uint32_t)p.otHour << 8) | p.otMin);
            fnvMix(h, ((uint32_t)p.depthRaw << 16) | ((uint32_t)p.magRaw << 8) | p.epicenterCode);
            fnvMix(h, ((uint32_t)p.latSouth << 31) | ((uint32_t)p.latDeg << 16) |
                      ((uint32_t)p.latMin << 8) | p.latSec);
            fnvMix(h, ((uint32_t)p.lonWest << 31) | ((uint32_t)p.lonDeg << 16) |
                      ((uint32_t)p.lonMin << 8) | p.lonSec);
            for (int i = 0; i < 3; i++) fnvMix(h, p.prevention[i]);
        }
    } else if (r.category == DCR_CAT_WEATHER) {
        DcrWeather w;
        if (dcr_weather(r, w)) {
            fnvMix(h, w.warningState);
            for (int i = 0; i < w.count; i++) {
                fnvMix(h, w.sub[i]);
                fnvMix(h, w.region[i]);
            }
        }
    } else if (r.category == DCR_CAT_SEISMIC) {
        DcrSeismic s;
        if (dcr_seismic(r, s)) {
            fnvMix(h, ((uint32_t)s.otDay << 16) | ((uint32_t)s.otHour << 8) | s.otMin);
            for (int i = 0; i < s.count; i++)
                fnvMix(h, ((uint32_t)s.intensity[i] << 8) | s.prefecture[i]);
        }
    } else if (r.category == DCR_CAT_NANKAI) {
        DcrNankai nk;
        if (dcr_nankai(r, nk)) {
            fnvMix(h, ((uint32_t)nk.infoSerialCode << 16) |
                      ((uint32_t)nk.pageNumber << 8) | nk.totalPage);
            // text[18] は 8bit×18＝144bit。4byteずつまとめて混ぜる（末尾の端数もそのまま）。
            for (int i = 0; i < 18; i += 4) {
                uint32_t v = 0;
                for (int j = 0; j < 4 && i + j < 18; j++) v = (v << 8) | nk.text[i + j];
                fnvMix(h, v);
            }
        }
    } else if (r.category == DCR_CAT_TSUNAMI) {
        DcrTsunami t;
        if (dcr_tsunami(r, t)) {
            fnvMix(h, t.warningCode);
            for (int i = 0; i < 3; i++) fnvMix(h, t.prevention[i]);
            for (int i = 0; i < t.count; i++) {
                fnvMix(h, ((uint32_t)t.nextDay[i] << 24) | ((uint32_t)t.arrHour[i] << 16) |
                          ((uint32_t)t.arrMin[i] << 8) | t.height[i]);
                fnvMix(h, t.region[i]);
            }
        }
    } else if (r.category == DCR_CAT_NWPAC_TSUNAMI) {
        DcrNwPacTsunami nt;
        if (dcr_nwpac_tsunami(r, nt)) {
            fnvMix(h, nt.potential);
            for (int i = 0; i < nt.count; i++) {
                fnvMix(h, ((uint32_t)nt.nextDay[i] << 24) | ((uint32_t)nt.arrHour[i] << 16) |
                          ((uint32_t)nt.arrMin[i] << 8) | nt.region[i]);
                fnvMix(h, nt.heightRaw[i]);
            }
        }
    } else if (r.category == DCR_CAT_VOLCANO) {
        DcrVolcano vo;
        if (dcr_volcano(r, vo)) {
            fnvMix(h, ((uint32_t)vo.ambiguityOfActivityTime << 24) |
                      ((uint32_t)vo.actDay << 16) | ((uint32_t)vo.actHour << 8) | vo.actMin);
            fnvMix(h, ((uint32_t)vo.warningCodeRaw << 16) | vo.volcanoNameRaw);
            for (int i = 0; i < vo.localGovCount; i++) fnvMix(h, vo.localGov[i]);
        }
    } else if (r.category == DCR_CAT_ASHFALL) {
        DcrAshFall af;
        if (dcr_ash_fall(r, af)) {
            fnvMix(h, ((uint32_t)af.actDay << 24) | ((uint32_t)af.actHour << 16) |
                      ((uint32_t)af.actMin << 8) | af.warningType);
            fnvMix(h, af.volcanoNameRaw);
            for (int i = 0; i < af.count; i++) {
                fnvMix(h, ((uint32_t)af.expectedTime[i] << 8) | af.warningCodeRaw[i]);
                fnvMix(h, af.localGov[i]);
            }
        }
    } else if (r.category == DCR_CAT_FLOOD) {
        DcrFlood fl;
        if (dcr_flood(r, fl)) {
            for (int i = 0; i < fl.count; i++) {
                fnvMix(h, fl.level[i]);
                // region は 40bit(uint64_t)。上下32bitに割って両方混ぜる。
                fnvMix(h, (uint32_t)(fl.region[i] >> 32));
                fnvMix(h, (uint32_t)(fl.region[i] & 0xFFFFFFFFu));
            }
        }
    } else if (r.category == DCR_CAT_TYPHOON) {
        DcrTyphoon ty;
        if (dcr_typhoon(r, ty)) {
            // ★台風は実況/予報レグが同じ報告時刻を共有し、位置・気圧・風速だけが
            //   違う複数メッセージが送られてくる。詳細を混ぜないと同一キーに
            //   丸め込まれてしまうので、全フィールドをキーに含める。
            fnvMix(h, ((uint32_t)ty.refDay << 24) | ((uint32_t)ty.refHour << 16) |
                      ((uint32_t)ty.refMin << 8) | ty.refTimeType);
            fnvMix(h, ((uint32_t)ty.elapsedFromRef << 24) | ((uint32_t)ty.typhoonNumber << 16) |
                      ((uint32_t)ty.scaleCategory << 8) | ty.intensityCategory);
            fnvMix(h, ((uint32_t)ty.latSouth << 31) | ((uint32_t)ty.latDeg << 16) |
                      ((uint32_t)ty.latMin << 8) | ty.latSec);
            fnvMix(h, ((uint32_t)ty.lonWest << 31) | ((uint32_t)ty.lonDeg << 16) |
                      ((uint32_t)ty.lonMin << 8) | ty.lonSec);
            fnvMix(h, ((uint32_t)ty.centralPressureRaw << 16) |
                      ((uint32_t)ty.maxWindSpeedRaw << 8) | ty.maxGustWindSpeedRaw);
        }
    } else if (r.category == DCR_CAT_MARINE) {
        DcrMarine ma;
        if (dcr_marine(r, ma)) {
            for (int i = 0; i < ma.count; i++)
                fnvMix(h, ((uint32_t)ma.code[i] << 16) | ma.region[i]);
        }
    }
    return h;
}

// ─────────────────────────────────────────────────────────────
// 緊急地震速報（disaster_category=1）詳細部
//
//   ビット配置（IS-QZSS-DCR §4.1.2.3.1）。
//   共通部 43bit の後、44〜46 は予備。以降は隙間なく連続する:
//     +47   3bit  長周期地震動階級 下限
//     +50   3bit  長周期地震動階級 上限
//     +53   9bit×3 防災上の留意事項（0 で打ち切り）        → 53..79
//     +80   5+5+6  地震発生時刻 日/時/分                   → 80..95
//     +96   9bit  深さ                                     → 96..104
//     +105  7bit  マグニチュード                           → 105..111
//     +112 10bit  震央地名コード                           → 112..121
//     +122  4bit  震度 下限 / +126 4bit 震度 上限          → 122..129
//     +130 80bit  対象地域ビットマップ（bit i → 地域コード i+1）→ 130..209
//   ★連続性（47→50→53→80→96→105→112→122→126→130→210）が隙間なく閉じることを
//     ビット配置を変更するときは必ず確認すること。
// ─────────────────────────────────────────────────────────────
bool dcr_eew(const DcrReport& r, DcrEew& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_EEW) return false;
    const uint8_t* m = r.raw;

    out.lgLower = dcr_bits(m, 47, 3);
    out.lgUpper = dcr_bits(m, 50, 3);

    for (int i = 0; i < 3; i++) {
        uint16_t v = dcr_bits(m, 53 + i * 9, 9);
        if (v == 0) break;               // 0 = 以降の留意事項なし
        out.prevention[i] = v;
    }

    out.otDay  = dcr_bits(m, 80, 5);
    out.otHour = dcr_bits(m, 85, 5);
    out.otMin  = dcr_bits(m, 90, 6);

    out.depthRaw      = dcr_bits(m, 96, 9);
    out.magRaw        = dcr_bits(m, 105, 7);
    out.epicenterCode = dcr_bits(m, 112, 10);
    out.intensityLower = dcr_bits(m, 122, 4);
    out.intensityUpper = dcr_bits(m, 126, 4);

    // 仮定震源要素: 震源を確定せず震度のみ推定している状態（深さ10km・M1.0 で埋める運用）
    out.assumptive = (out.depthRaw == 10 && out.magRaw == 10);

    for (int i = 0; i < 80; i++) {
        if (dcr_bits(m, 130 + i, 1)) {
            out.regionBits[i >> 3] |= (uint8_t)(1u << (i & 7));
            out.regionCount++;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 震源（disaster_category=2）詳細部
//
//   ビット配置:
//   bit121 までは EEW と完全に同一で、+122 から分岐する:
//     +53   9bit×3 防災上の留意事項      → 53..79
//     +80   5+5+6  地震発生時刻 日/時/分  → 80..95
//     +96   9bit  深さ / +105 7bit M / +112 10bit 震央地名コード → 96..121
//     +122 41bit  実座標                 → 122..162
//        緯度: 1bit N/S(0=北) + 7bit 度(0-89) + 6bit 分 + 6bit 秒
//        経度: 1bit E/W(0=東) + 8bit 度(0-179) + 6bit 分 + 6bit 秒
//   ★EEW(1) は +122 が予想震度＋地域ビットマップで座標を持たない。ここを取り違えると
//     「震度コードを緯度として読む」形で静かに壊れる＝必ず category で分岐すること。
// ─────────────────────────────────────────────────────────────
bool dcr_hypocenter(const DcrReport& r, DcrHypocenter& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_HYPOCENTER) return false;
    const uint8_t* m = r.raw;

    for (int i = 0; i < 3; i++) {
        uint16_t v = dcr_bits(m, 53 + i * 9, 9);
        if (v == 0) break;
        out.prevention[i] = v;
    }
    out.otDay  = dcr_bits(m, 80, 5);
    out.otHour = dcr_bits(m, 85, 5);
    out.otMin  = dcr_bits(m, 90, 6);

    out.depthRaw      = dcr_bits(m, 96, 9);
    out.magRaw        = dcr_bits(m, 105, 7);
    out.epicenterCode = dcr_bits(m, 112, 10);

    out.latSouth = dcr_bits(m, 122, 1) != 0;   // 0=北緯 / 1=南緯
    out.latDeg   = dcr_bits(m, 123, 7);
    out.latMin   = dcr_bits(m, 130, 6);
    out.latSec   = dcr_bits(m, 136, 6);
    out.lonWest  = dcr_bits(m, 142, 1) != 0;   // 0=東経 / 1=西経
    out.lonDeg   = dcr_bits(m, 143, 8);
    out.lonMin   = dcr_bits(m, 151, 6);
    out.lonSec   = dcr_bits(m, 157, 6);

    // 度分秒 → 十進度。仕様上あり得ない値（度が範囲外）は 0 のままにせず素直に
    // 変換する（受信側で弾くと「壊れた通報が無かったこと」になるため）。異常判定は
    // 呼び出し側の責務＝latDeg<=89 / lonDeg<=179 を見ればよい。
    out.latitude  = (double)out.latDeg + out.latMin / 60.0 + out.latSec / 3600.0;
    out.longitude = (double)out.lonDeg + out.lonMin / 60.0 + out.lonSec / 3600.0;
    if (out.latSouth) out.latitude  = -out.latitude;
    if (out.lonWest)  out.longitude = -out.longitude;
    return true;
}

bool dcr_eew_has_region(const DcrEew& e, uint8_t regionCode) {
    if (regionCode < 1 || regionCode > 80) return false;
    int i = regionCode - 1;
    return (e.regionBits[i >> 3] >> (i & 7)) & 1;
}

// ─────────────────────────────────────────────────────────────
// 震度（disaster_category=3）詳細部
//
//   ビット配置:
//     +53   5+5+6  地震発生時刻 日/時/分                → 53..68
//     +69   9bit×最大16（震度3bit ＋ 都道府県6bit）      → 69..212
//        両方 0 で打ち切り
//
//   ★震度コードは **3bit の専用表**（1=4未満 … 7=震度7）。EEW の予想震度は 4bit の
//     別表で、値の意味が全く違う。取り違えると平然と別の震度を表示するので注意。
//     関数も dcr_seismic_intensity_name / dcr_intensity_name で分けてある。
// ─────────────────────────────────────────────────────────────
bool dcr_seismic(const DcrReport& r, DcrSeismic& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_SEISMIC) return false;
    const uint8_t* m = r.raw;

    out.otDay  = dcr_bits(m, 53, 5);
    out.otHour = dcr_bits(m, 58, 5);
    out.otMin  = dcr_bits(m, 63, 6);

    for (int i = 0; i < 16; i++) {
        int offset = 69 + i * 9;
        uint8_t es = dcr_bits(m, offset, 3);
        uint8_t pl = dcr_bits(m, offset + 3, 6);
        if (es == 0 && pl == 0) break;
        out.intensity[out.count]  = es;
        out.prefecture[out.count] = pl;
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 津波（disaster_category=5）詳細部
//
//   ビット配置:
//     +53   9bit×3  防災上の留意事項（0で打ち切り）       → 53..79
//     +80   4bit    津波警報コード                        → 80..83
//     +84   26bit×最大5（26bit 全体が 0 で打ち切り）      → 84..213
//        +0   1bit  到達予想が翌日か（報告日からのオフセット）
//        +1   5bit  到達予想 時（31=不明）
//        +6   6bit  到達予想 分（63=不明）
//        +12  4bit  津波の高さ
//        +16 10bit  津波予報区
//
//   ★「不明」を表す 31/63 はそのまま保持する。ここで 0 などへ丸めると
//     「0時0分に到達」と誤読される（災害情報でこれは危険）。判断は表示側の責務。
// ─────────────────────────────────────────────────────────────
bool dcr_tsunami(const DcrReport& r, DcrTsunami& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_TSUNAMI) return false;
    const uint8_t* m = r.raw;

    for (int i = 0; i < 3; i++) {
        uint16_t v = dcr_bits(m, 53 + i * 9, 9);
        if (v == 0) break;
        out.prevention[i] = v;
    }
    out.warningCode = dcr_bits(m, 80, 4);

    for (int i = 0; i < 5; i++) {
        int offset = 84 + i * 26;
        // 26bit まるごと 0 なら以降なし。★dcr_bits は最大32bitまで読める
        if (dcr_bits(m, offset, 26) == 0) break;
        out.nextDay[out.count] = dcr_bits(m, offset, 1);
        out.arrHour[out.count] = dcr_bits(m, offset + 1, 5);
        out.arrMin[out.count]  = dcr_bits(m, offset + 6, 6);
        out.height[out.count]  = dcr_bits(m, offset + 12, 4);
        out.region[out.count]  = dcr_bits(m, offset + 16, 10);
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 気象（disaster_category=10）詳細部
//   ビット配置（IS-QZSS-DCR §4.1.2.3.10）。
//     +53  3bit  警戒状態（発表/解除）
//     +56〜 24bit×最大6組（0 で打ち切り）: 5bit サブカテゴリ ＋ 19bit 府県予報区
// ─────────────────────────────────────────────────────────────
bool dcr_weather(const DcrReport& r, DcrWeather& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_WEATHER) return false;
    const uint8_t* m = r.raw;

    out.warningState = dcr_bits(m, 53, 3);
    for (int i = 0; i < 6; i++) {
        int offset = 56 + i * 24;
        if (dcr_bits(m, offset, 24) == 0) break;
        out.sub[out.count]    = dcr_bits(m, offset, 5);
        out.region[out.count] = dcr_bits(m, offset + 5, 19);
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 南海トラフ地震関連情報（disaster_category=4）詳細部
//   ビット配置:
//     +53  4bit  情報種類コード                    → 53..56
//     +57  8bit×18  テキスト情報（生バイト）        → 57..200
//     +201 6bit  ページ番号 / +207 6bit 総ページ数  → 201..212
//   214bit目からversionフィールドという上位仕様と、+207+6=213 で綺麗に閉じる
//   ことを確認済み（1bit の余白なし）。
// ─────────────────────────────────────────────────────────────
bool dcr_nankai(const DcrReport& r, DcrNankai& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_NANKAI) return false;
    const uint8_t* m = r.raw;

    out.infoSerialCode = dcr_bits(m, 53, 4);
    for (int i = 0; i < 18; i++) out.text[i] = (uint8_t)dcr_bits(m, 57 + i * 8, 8);
    out.pageNumber = dcr_bits(m, 201, 6);
    out.totalPage  = dcr_bits(m, 207, 6);
    return true;
}

// ─────────────────────────────────────────────────────────────
// 北西太平洋津波（disaster_category=6）詳細部
//   ビット配置:
//     +53  3bit  津波発生可能性                              → 53..55
//     +56  28bit×最大5（28bit まるごと0で打ち切り）           → 56..195
//        +0  1bit  到達予想が翌日か
//        +1  5bit  到達予想 時（31=不明）
//        +6  6bit  到達予想 分（63=不明）
//        +12 9bit  津波高さ
//        +21 7bit  沿岸地域コード
// ─────────────────────────────────────────────────────────────
bool dcr_nwpac_tsunami(const DcrReport& r, DcrNwPacTsunami& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_NWPAC_TSUNAMI) return false;
    const uint8_t* m = r.raw;

    out.potential = dcr_bits(m, 53, 3);
    for (int i = 0; i < 5; i++) {
        int offset = 56 + i * 28;
        if (dcr_bits(m, offset, 28) == 0) break;
        out.nextDay[out.count]   = dcr_bits(m, offset, 1) != 0;
        out.arrHour[out.count]   = dcr_bits(m, offset + 1, 5);
        out.arrMin[out.count]    = dcr_bits(m, offset + 6, 6);
        out.heightRaw[out.count] = dcr_bits(m, offset + 12, 9);
        out.region[out.count]    = dcr_bits(m, offset + 21, 7);
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 火山（disaster_category=8）詳細部
//   ビット配置:
//     +50  3bit  活動時刻の曖昧性                    → 50..52
//     +53  5+5+6  活動時刻 日/時/分                  → 53..68
//     +69  7bit  火山警戒コード                       → 69..75
//     +76  12bit 火山名コード                         → 76..87
//     +88  23bit×最大5（0で打ち切り） 市町村コード     → 88..202
// ─────────────────────────────────────────────────────────────
bool dcr_volcano(const DcrReport& r, DcrVolcano& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_VOLCANO) return false;
    const uint8_t* m = r.raw;

    out.ambiguityOfActivityTime = dcr_bits(m, 50, 3);
    out.actDay  = dcr_bits(m, 53, 5);
    out.actHour = dcr_bits(m, 58, 5);
    out.actMin  = dcr_bits(m, 63, 6);
    out.warningCodeRaw = dcr_bits(m, 69, 7);
    out.volcanoNameRaw = dcr_bits(m, 76, 12);

    for (int i = 0; i < 5; i++) {
        int offset = 88 + i * 23;
        uint32_t lg = dcr_bits(m, offset, 23);
        if (lg == 0) break;
        out.localGov[out.localGovCount++] = lg;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 降灰（disaster_category=9）詳細部
//   ビット配置:
//     +53  5+5+6  活動時刻 日/時/分                  → 53..68
//     +69  2bit  速報/詳細種別                        → 69..70
//     +71  12bit 火山名コード（火山カテゴリと共用の表） → 71..82
//     +83  29bit×最大4（0で打ち切り）                 → 83..198
//        +0  3bit 予想降灰時間帯コード
//        +3  3bit 降灰警報コード
//        +6  23bit 市町村コード
// ─────────────────────────────────────────────────────────────
bool dcr_ash_fall(const DcrReport& r, DcrAshFall& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_ASHFALL) return false;
    const uint8_t* m = r.raw;

    out.actDay  = dcr_bits(m, 53, 5);
    out.actHour = dcr_bits(m, 58, 5);
    out.actMin  = dcr_bits(m, 63, 6);
    out.warningType    = dcr_bits(m, 69, 2);
    out.volcanoNameRaw = dcr_bits(m, 71, 12);

    for (int i = 0; i < 4; i++) {
        int offset = 83 + i * 29;
        if (dcr_bits(m, offset, 29) == 0) break;
        out.expectedTime[out.count]   = dcr_bits(m, offset, 3);
        out.warningCodeRaw[out.count] = dcr_bits(m, offset + 3, 3);
        out.localGov[out.count]       = dcr_bits(m, offset + 6, 23);
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 洪水（disaster_category=11）詳細部
//   ビット配置:
//     +53  44bit×最大3（44bit まるごと0で打ち切り）
//        +0  4bit  警戒レベル
//        +4  40bit 洪水予報区（河川ごとの細粒度コード＝uint64_tで保持）
// ─────────────────────────────────────────────────────────────
bool dcr_flood(const DcrReport& r, DcrFlood& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_FLOOD) return false;
    const uint8_t* m = r.raw;

    for (int i = 0; i < 3; i++) {
        int offset = 53 + i * 44;
        // 44bit は dcr_bits(32bit上限)を跨ぐため、上位4bit(level)と下位40bitを分けて読む。
        uint8_t lv = (uint8_t)dcr_bits(m, offset, 4);
        // 40bitは32bit超なので上位8bit＋下位32bitに分割して結合する。
        uint64_t hi = dcr_bits(m, offset + 4, 8);
        uint64_t lo = dcr_bits(m, offset + 12, 32);
        uint64_t region = (hi << 32) | lo;
        if (lv == 0 && region == 0) break;
        out.level[out.count]  = lv;
        out.region[out.count] = region;
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// 台風（disaster_category=12）詳細部
//   ビット配置:
//     +53  5+5+6  解析基点時刻 日/時/分            → 53..68
//     +69  3bit  基点時刻種別                      → 69..71
//     +80  7bit  基点からの経過時間                 → 80..86
//     +87  7bit  台風番号(1〜99)                    → 87..93
//     +94  4bit  大きさ階級 / +98 4bit 強さ階級     → 94..101
//     +102 41bit 実座標（震源・緯度経度と同形式）     → 102..142
//     +143 11bit 中心気圧 / +154 7bit 最大風速 / +161 7bit 最大瞬間風速 → 143..167
// ─────────────────────────────────────────────────────────────
bool dcr_typhoon(const DcrReport& r, DcrTyphoon& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_TYPHOON) return false;
    const uint8_t* m = r.raw;

    out.refDay  = dcr_bits(m, 53, 5);
    out.refHour = dcr_bits(m, 58, 5);
    out.refMin  = dcr_bits(m, 63, 6);
    out.refTimeType    = dcr_bits(m, 69, 3);
    out.elapsedFromRef = dcr_bits(m, 80, 7);
    out.typhoonNumber  = dcr_bits(m, 87, 7);
    out.scaleCategory     = dcr_bits(m, 94, 4);
    out.intensityCategory = dcr_bits(m, 98, 4);

    out.latSouth = dcr_bits(m, 102, 1) != 0;
    out.latDeg   = dcr_bits(m, 103, 7);
    out.latMin   = dcr_bits(m, 110, 6);
    out.latSec   = dcr_bits(m, 116, 6);
    out.lonWest  = dcr_bits(m, 122, 1) != 0;
    out.lonDeg   = dcr_bits(m, 123, 8);
    out.lonMin   = dcr_bits(m, 131, 6);
    out.lonSec   = dcr_bits(m, 137, 6);
    out.latitude  = (double)out.latDeg + out.latMin / 60.0 + out.latSec / 3600.0;
    out.longitude = (double)out.lonDeg + out.lonMin / 60.0 + out.lonSec / 3600.0;
    if (out.latSouth) out.latitude  = -out.latitude;
    if (out.lonWest)  out.longitude = -out.longitude;

    out.centralPressureRaw   = dcr_bits(m, 143, 11);
    out.maxWindSpeedRaw      = dcr_bits(m, 154, 7);
    out.maxGustWindSpeedRaw  = dcr_bits(m, 161, 7);
    return true;
}

// ─────────────────────────────────────────────────────────────
// 海上（disaster_category=14）詳細部
//   ビット配置:
//     +53  19bit×最大8（コード0かつ地域0で打ち切り）
//        +0  5bit  海上警報コード（0=解除）
//        +5  14bit 地方海上予報区
// ─────────────────────────────────────────────────────────────
bool dcr_marine(const DcrReport& r, DcrMarine& out) {
    memset(&out, 0, sizeof(out));
    if (!r.valid || !r.jmaValid || r.category != DCR_CAT_MARINE) return false;
    const uint8_t* m = r.raw;

    for (int i = 0; i < 8; i++) {
        int offset = 53 + i * 19;
        uint8_t  dw = (uint8_t)dcr_bits(m, offset, 5);
        uint16_t pl = (uint16_t)dcr_bits(m, offset + 5, 14);
        if (dw == 0 && pl == 0) break;
        out.code[out.count]   = dw;
        out.region[out.count] = pl;
        out.count++;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────
// コード → 名称テーブル
// ─────────────────────────────────────────────────────────────
// ── 通報区分（Rc・3bit） ────────────────────────────────────────────────────
//   IS-QZSS-DCR-016 Table 4.1.2-5 他（各カテゴリの Parameter Definitions）:
//     Rc: 1=Maximum priority / 2=Priority / 3=Regular / 7=Training/Test
//     （0,4,5,6 は仕様上未割当）
//
//   ★訓練/試験(7)は優先度の序列の外にある別軸のマーカーであり、「通常より下の
//     優先度」として扱ってはいけない（送信頻度は「優先」と同等＝低優先ではない）。
//     誤った対応表を実装して実災害を「訓練」と誤判定した実例は
//     docs/DECODER_NOTES.md ④ 参照。
//
//   【カテゴリごとに取り得る値が違う】IS-QZSS-DCR-016 各 Parameter Definitions の
//     Effective Range より（受信側で検証に使える）:
//       EEW=1,7 ／ 震源・震度・南海トラフ・北西太平洋津波・降灰=2,7
//       津波=1,3,7 ／ 火山・気象・洪水=2,3,7 ／ 台風・海上=3,7
const char* dcr_classification_name(uint8_t rc) {
    switch (rc) {
        case 1: return "最優先";
        case 2: return "優先";
        case 3: return "通常";
        case 7: return "訓練/試験";
        default: return "?";   // 0,4,5,6 は仕様上未割当
    }
}

const char* dcr_category_name(uint8_t dc) {
    switch (dc) {
        case 1:  return "緊急地震速報";
        case 2:  return "震源";
        case 3:  return "震度";
        case 4:  return "南海トラフ地震";
        case 5:  return "津波";
        case 6:  return "北西太平洋津波";
        case 8:  return "火山";
        case 9:  return "降灰";
        case 10: return "気象";
        case 11: return "洪水";
        case 12: return "台風";
        case 14: return "海上";
        default: return "?";
    }
}

// 情報種別（It・2bit）。★「取消」が定義されていること自体が、DC Report が
//   「いま有効な通報の掲示板」であることの根拠（発表から取消まで載り続ける）。
const char* dcr_info_type_name(uint8_t it) {
    switch (it) {
        case 0: return "発表";
        case 1: return "訂正";
        case 2: return "取消";
        default: return "?";
    }
}

// 震度（下限・上限共用）。上限のみ 11=「〜程度以上」を取り得る。
const char* dcr_intensity_name(uint8_t v) {
    switch (v) {
        case 1:  return "震度0";
        case 2:  return "震度1";
        case 3:  return "震度2";
        case 4:  return "震度3";
        case 5:  return "震度4";
        case 6:  return "震度5弱";
        case 7:  return "震度5強";
        case 8:  return "震度6弱";
        case 9:  return "震度6強";
        case 10: return "震度7";
        case 11: return "〜程度以上";
        case 14: return "なし";
        case 15: return "不明";
        default: return "?";
    }
}

// 震度カテゴリ(3)の観測震度。★3bit の専用表。EEW の 4bit 表とは別物。
const char* dcr_seismic_intensity_name(uint8_t v) {
    switch (v) {
        case 1: return "震度4未満";
        case 2: return "震度4";
        case 3: return "震度5弱";
        case 4: return "震度5強";
        case 5: return "震度6弱";
        case 6: return "震度6強";
        case 7: return "震度7";
        default: return "?";
    }
}

// 都道府県（6bit・47件）。コード順＝北から南（気象庁の標準順）。
static const char* const kPrefectures[] = {
    "北海道","青森県","岩手県","宮城県","秋田県","山形県","福島県",
    "茨城県","栃木県","群馬県","埼玉県","千葉県","東京都","神奈川県",
    "新潟県","富山県","石川県","福井県","山梨県","長野県","岐阜県",
    "静岡県","愛知県","三重県","滋賀県","京都府","大阪府","兵庫県",
    "奈良県","和歌山県","鳥取県","島根県","岡山県","広島県","山口県",
    "徳島県","香川県","愛媛県","高知県","福岡県","佐賀県","長崎県",
    "熊本県","大分県","宮崎県","鹿児島県","沖縄県",
};
const char* dcr_prefecture_name(uint8_t code) {
    if (code >= 1 && code <= 47) return kPrefectures[code - 1];
    return "?";
}

const char* dcr_tsunami_warning_name(uint8_t v) {
    switch (v) {
        case 1:  return "津波なし";
        case 2:  return "警報解除";
        case 3:  return "津波警報";
        case 4:  return "大津波警報";
        case 5:  return "大津波警報：発表";
        case 15: return "その他の警報";
        default: return "?";
    }
}

const char* dcr_tsunami_height_name(uint8_t v) {
    switch (v) {
        case 1:  return "0.2m未満";
        case 2:  return "1m";
        case 3:  return "3m";
        case 4:  return "5m";
        case 5:  return "10m";
        case 6:  return "10m超";
        case 14: return "不明";
        case 15: return "その他の津波の高さ";
        default: return "?";
    }
}

// 長周期地震動階級（EEW の LgL1/LgU1・各 3bit）。
//   ★**上限のみ 6="〜程度以上" を取り得る**（一次資料 Table 4.1.2-11-1 が下限で 6 を
//     持たず、4.1.2-11-2 が上限で持つ）。予想震度が上限のみ 11="〜程度以上" を持つのと
//     全く同じ構造。震度側は当初から対応していたのに、こちらは 6 が抜けていて
//     上限 6 のとき "?" を表示していた（2026-07-30・一次資料との機械照合で発見）。
//   実装は上下限で 1 つの関数を共用するため、和集合（0〜7）を持つ。
const char* dcr_long_period_name(uint8_t v) {
    switch (v) {
        case 0: return "";                      // 設定なし（一次資料本文「データが無い場合は0」）
        case 1: return "長周期地震動階級1未満";
        case 2: return "長周期地震動階級1";
        case 3: return "長周期地震動階級2";
        case 4: return "長周期地震動階級3";
        case 5: return "長周期地震動階級4";
        case 6: return "〜程度以上";            // ★上限のみ。下限では出ない
        case 7: return "不明";
        default: return "?";
    }
}

// EEW 対象地域（80bit ビットマップのコード 1〜80）
static const char* const kEewRegions[] = {
    /* 1*/"北海道道央",/* 2*/"北海道道南",/* 3*/"北海道道北",/* 4*/"北海道道東",
    /* 5*/"青森",/* 6*/"岩手",/* 7*/"宮城",/* 8*/"秋田",/* 9*/"山形",/*10*/"福島",
    /*11*/"茨城",/*12*/"栃木",/*13*/"群馬",/*14*/"埼玉",/*15*/"千葉",/*16*/"東京",
    /*17*/"伊豆諸島",/*18*/"小笠原",/*19*/"神奈川",/*20*/"新潟",/*21*/"富山",
    /*22*/"石川",/*23*/"福井",/*24*/"山梨",/*25*/"長野",/*26*/"岐阜",/*27*/"静岡",
    /*28*/"愛知",/*29*/"三重",/*30*/"滋賀",/*31*/"京都",/*32*/"大阪",/*33*/"兵庫",
    /*34*/"奈良",/*35*/"和歌山",/*36*/"鳥取",/*37*/"島根",/*38*/"岡山",/*39*/"広島",
    /*40*/"山口",/*41*/"徳島",/*42*/"香川",/*43*/"愛媛",/*44*/"高知",/*45*/"福岡",
    /*46*/"佐賀",/*47*/"長崎",/*48*/"熊本",/*49*/"大分",/*50*/"宮崎",/*51*/"鹿児島",
    /*52*/"奄美(群島)",/*53*/"沖縄本島",/*54*/"大東島",/*55*/"宮古島",/*56*/"八重山",
    // 57〜70 は地方予報区（都道府県より広い単位）
    /*57*/"北海道",/*58*/"東北",/*59*/"関東",/*60*/"伊豆諸島",/*61*/"小笠原",
    /*62*/"北陸",/*63*/"甲信",/*64*/"東海",/*65*/"近畿",/*66*/"中国",/*67*/"四国",
    /*68*/"九州",/*69*/"奄美(群島)",/*70*/"沖縄",
};
static const int kEewRegionCount = sizeof(kEewRegions) / sizeof(kEewRegions[0]);

const char* dcr_eew_region_name(uint8_t code) {
    if (code >= 1 && code <= kEewRegionCount) return kEewRegions[code - 1];
    if (code == 80) return "その他の府県予報区および地方予報区";
    return "?";   // 71〜79 は未定義（仕様上の空き）
}

const char* dcr_weather_state_name(uint8_t v) {
    switch (v) {
        case 1: return "発表";
        case 2: return "解除";
        default: return "?";
    }
}

const char* dcr_weather_sub_name(uint8_t v) {
    switch (v) {
        case 1:  return "暴風雪特別警報";
        case 2:  return "大雨特別警報";
        case 3:  return "暴風特別警報";
        case 4:  return "大雪特別警報";
        case 5:  return "波浪特別警報";
        case 6:  return "高潮特別警報";
        case 7:  return "全ての気象特別警報";
        case 21: return "記録的短時間大雨情報";
        case 22: return "竜巻注意情報";
        case 23: return "土砂災害警戒情報";
        case 31: return "その他の警報等情報要素";
        default: return "?";
    }
}

// 府県予報区コード（気象庁）。IS-QZSS-DCR 表 4.1.2-41 と全件一致確認済み・78件。
struct WeatherRegion { uint32_t code; const char* name; };
static const WeatherRegion kWeatherRegions[] = {
    {11000,"宗谷地方"},{12000,"上川・留萌地方"},{12010,"上川地方"},{12020,"留萌地方"},
    {13000,"網走・北見・紋別地方"},{14010,"根室地方"},{14020,"釧路地方"},{14030,"十勝地方"},
    {14100,"釧路・根室地方"},{15000,"胆振・日高地方"},{15010,"胆振地方"},{15020,"日高地方"},
    {16000,"石狩・空知・後志地方"},{16010,"石狩地方"},{16020,"空知地方"},{16030,"後志地方"},
    {16100,"石狩・空知地方"},{17000,"渡島・檜山地方"},{17010,"渡島地方"},{17020,"檜山地方"},
    {20000,"青森県"},{30000,"岩手県"},{40000,"宮城県"},{50000,"秋田県"},{60000,"山形県"},
    {70000,"福島県"},{80000,"茨城県"},{90000,"栃木県"},{100000,"群馬県"},{110000,"埼玉県"},
    {120000,"千葉県"},{130000,"東京都"},{130010,"東京地方"},{130020,"伊豆諸島北部"},
    {130030,"伊豆諸島南部"},{140000,"神奈川県"},{150000,"新潟県"},{160000,"富山県"},
    {170000,"石川県"},{180000,"福井県"},{190000,"山梨県"},{200000,"長野県"},{210000,"岐阜県"},
    {220000,"静岡県"},{230000,"愛知県"},{240000,"三重県"},{250000,"滋賀県"},{260000,"京都府"},
    {270000,"大阪府"},{280000,"兵庫県"},{290000,"奈良県"},{300000,"和歌山県"},{310000,"鳥取県"},
    {320000,"島根県"},{330000,"岡山県"},{340000,"広島県"},{350000,"山口県"},{360000,"徳島県"},
    {370000,"香川県"},{380000,"愛媛県"},{390000,"高知県"},{400000,"福岡県"},{410000,"佐賀県"},
    {420000,"長崎県"},{430000,"熊本県"},{440000,"大分県"},{450000,"宮崎県"},{460000,"鹿児島県"},
    {460040,"奄美地方"},{460100,"鹿児島県(奄美地方除く)"},{471000,"沖縄本島地方"},
    {472000,"大東島地方"},{473000,"宮古島地方"},{474000,"八重山地方"},{500000,"その他の府県予報区"},
};
static const int kWeatherRegionCount = sizeof(kWeatherRegions) / sizeof(kWeatherRegions[0]);

const char* dcr_weather_region_name(uint32_t code) {
    for (int i = 0; i < kWeatherRegionCount; i++)
        if (kWeatherRegions[i].code == code) return kWeatherRegions[i].name;
    return "?";
}

// ─────────────────────────────────────────────────────────────
// 1行要約（時刻は JST へ変換して出す）
//   ログ・表示・シリアル出力など複数箇所で同じ文面を使えるようにここへ置く。
// ─────────────────────────────────────────────────────────────
void dcr_summary(const DcrReport& r, char* buf, size_t n) {
    char dbuf[16], mbuf[16];
    if (r.category == DCR_CAT_EEW) {
        DcrEew e;
        if (dcr_eew(r, e)) {
            uint8_t mo = r.month, d = e.otDay, hh = e.otHour, mi = e.otMin;
            dcr_to_jst(&mo, &d, &hh, &mi);   // 電文は UTC
            snprintf(buf, n, "EEW %02u:%02u %s %s %s 震度%s〜%s%s",
                     hh, mi,
                     dcr_epicenter_name(e.epicenterCode),
                     dcr_depth_text(e.depthRaw, dbuf, sizeof(dbuf)),
                     dcr_magnitude_text(e.magRaw, mbuf, sizeof(mbuf)),
                     dcr_intensity_name(e.intensityLower),
                     dcr_intensity_name(e.intensityUpper),
                     e.assumptive ? " 仮定震源" : "");
            return;
        }
    } else if (r.category == DCR_CAT_HYPOCENTER) {
        DcrHypocenter h;
        if (dcr_hypocenter(r, h)) {
            uint8_t mo = r.month, d = h.otDay, hh = h.otHour, mi = h.otMin;
            dcr_to_jst(&mo, &d, &hh, &mi);
            snprintf(buf, n, "震源 %02u:%02u %s %s %s (%.4f,%.4f)",
                     hh, mi,
                     dcr_epicenter_name(h.epicenterCode),
                     dcr_depth_text(h.depthRaw, dbuf, sizeof(dbuf)),
                     dcr_magnitude_text(h.magRaw, mbuf, sizeof(mbuf)),
                     h.latitude, h.longitude);
            return;
        }
    } else if (r.category == DCR_CAT_SEISMIC) {
        DcrSeismic s;
        if (dcr_seismic(r, s)) {
            uint8_t mo = r.month, d = s.otDay, hh = s.otHour, mi = s.otMin;
            dcr_to_jst(&mo, &d, &hh, &mi);
            snprintf(buf, n, "震度 %02u:%02u %s %s%s", hh, mi,
                     s.count > 0 ? dcr_prefecture_name(s.prefecture[0]) : "-",
                     s.count > 0 ? dcr_seismic_intensity_name(s.intensity[0]) : "-",
                     s.count > 1 ? " ほか" : "");
            return;
        }
    } else if (r.category == DCR_CAT_TSUNAMI) {
        DcrTsunami t;
        if (dcr_tsunami(r, t)) {
            snprintf(buf, n, "津波 %s %s %s%s", dcr_tsunami_warning_name(t.warningCode),
                     t.count > 0 ? dcr_tsunami_region_name(t.region[0]) : "-",
                     t.count > 0 ? dcr_tsunami_height_name(t.height[0]) : "-",
                     t.count > 1 ? " ほか" : "");
            return;
        }
    } else if (r.category == DCR_CAT_WEATHER) {
        DcrWeather w;
        if (dcr_weather(r, w)) {
            snprintf(buf, n, "気象 %s %s:%s%s",
                     dcr_weather_state_name(w.warningState),
                     w.count > 0 ? dcr_weather_region_name(w.region[0]) : "-",
                     w.count > 0 ? dcr_weather_sub_name(w.sub[0]) : "-",
                     w.count > 1 ? " ほか" : "");
            return;
        }
    } else if (r.category == DCR_CAT_NANKAI) {
        DcrNankai nk;
        if (dcr_nankai(r, nk)) {
            snprintf(buf, n, "南海トラフ %s (%u/%u頁)",
                     dcr_nankai_serial_code_name(nk.infoSerialCode),
                     nk.pageNumber, nk.totalPage);
            return;
        }
    } else if (r.category == DCR_CAT_NWPAC_TSUNAMI) {
        DcrNwPacTsunami nt;
        if (dcr_nwpac_tsunami(r, nt)) {
            snprintf(buf, n, "北西太平洋津波 %s %s%s",
                     dcr_tsunamigenic_potential_name(nt.potential),
                     nt.count > 0 ? dcr_coastal_region_name(nt.region[0]) : "-",
                     nt.count > 1 ? " ほか" : "");
            return;
        }
    } else if (r.category == DCR_CAT_VOLCANO) {
        DcrVolcano vo;
        if (dcr_volcano(r, vo)) {
            snprintf(buf, n, "火山 %s %s",
                     dcr_volcano_name(vo.volcanoNameRaw),
                     dcr_volcanic_warning_code_name(vo.warningCodeRaw));
            return;
        }
    } else if (r.category == DCR_CAT_ASHFALL) {
        DcrAshFall af;
        if (dcr_ash_fall(r, af)) {
            snprintf(buf, n, "降灰 %s %s%s",
                     dcr_volcano_name(af.volcanoNameRaw),
                     af.count > 0 ? dcr_ash_fall_warning_code_name(af.warningCodeRaw[0]) : "-",
                     af.count > 1 ? " ほか" : "");
            return;
        }
    } else if (r.category == DCR_CAT_FLOOD) {
        DcrFlood fl;
        if (dcr_flood(r, fl)) {
            snprintf(buf, n, "洪水 %s %s%s",
                     fl.count > 0 ? dcr_flood_warning_level_name(fl.level[0]) : "-",
                     fl.count > 0 ? dcr_flood_forecast_region_name(fl.region[0]) : "-",
                     fl.count > 1 ? " ほか" : "");
            return;
        }
    } else if (r.category == DCR_CAT_TYPHOON) {
        DcrTyphoon ty;
        if (dcr_typhoon(r, ty)) {
            snprintf(buf, n, "台風%u号 %s%s (%.1f,%.1f)",
                     ty.typhoonNumber,
                     dcr_typhoon_scale_category_name(ty.scaleCategory),
                     dcr_typhoon_intensity_category_name(ty.intensityCategory),
                     ty.latitude, ty.longitude);
            return;
        }
    } else if (r.category == DCR_CAT_MARINE) {
        DcrMarine ma;
        if (dcr_marine(r, ma)) {
            snprintf(buf, n, "海上 %s %s%s",
                     ma.count > 0 ? dcr_marine_warning_code_name(ma.code[0]) : "-",
                     ma.count > 0 ? dcr_marine_forecast_region_name(ma.region[0]) : "-",
                     ma.count > 1 ? " ほか" : "");
            return;
        }
    }
    // 詳細デコーダが無いカテゴリ ＝ カテゴリ名＋発令時刻（共通部だけで作れる情報）
    uint8_t mo = r.month, d = r.day, hh = r.hour, mi = r.minute;
    dcr_to_jst(&mo, &d, &hh, &mi);
    snprintf(buf, n, "%s %u/%u %02u:%02u", dcr_category_name(r.category), mo, d, hh, mi);
}

// ─────────────────────────────────────────────────────────────
// 2026-07-30 追加：残り7カテゴリの名称テーブル（小さい表のみここに直置き。
//   大きい表は dcr_local_government_table.cpp / dcr_flood_region_table.cpp /
//   dcr_volcano_name_table.cpp / dcr_coastal_region_table.cpp へ分離済み）。
// ─────────────────────────────────────────────────────────────
const char* dcr_nankai_serial_code_name(uint8_t v) {
    switch (v) {
        case 1:  return "調査中A(M6.8以上)";
        case 2:  return "調査中B(ひずみ計の有意変化)";
        case 3:  return "調査中C(その他の関連現象)";
        case 4:  return "巨大地震警戒";
        case 5:  return "巨大地震注意";
        case 6:  return "調査終了";
        case 15: return "その他の情報";
        default: return "?";
    }
}

const char* dcr_tsunamigenic_potential_name(uint8_t v) {
    switch (v) {
        case 0: return "津波の可能性なし";
        case 1: return "太平洋広域の破壊的な津波の可能性";
        case 2: return "周辺地域の破壊的な津波の可能性";
        case 3: return "震源域近傍の破壊的な津波の可能性";
        case 4: return "震源域近傍の小規模な津波の可能性(僅か)";
        case 7: return "その他の津波発生の可能性";
        default: return "?";
    }
}

const char* dcr_nwpac_tsunami_height_name(uint16_t v) {
    switch (v) {
        case 1:   return "0.3m〜1m";
        case 2:   return "1m〜3m";
        case 3:   return "3m〜5m";
        case 4:   return "5m〜10m";
        case 508: return "10m超";
        case 509: return "巨大";
        case 510: return "高い";
        case 511: return "不明";
        default:  return "?";
    }
}

const char* dcr_volcanic_warning_code_name(uint8_t v) {
    switch (v) {
        case 11: return "レベル1(活火山であることに留意)";
        case 12: return "レベル2(火口周辺規制)";
        case 13: return "レベル3(入山規制)";
        case 14: return "レベル4(高齢者等避難)";
        case 15: return "レベル5(避難)";
        case 21: return "活火山であることに留意";
        case 22: return "火口周辺危険";
        case 23: return "入山危険";
        case 24: return "山麓厳重警戒";
        case 25: return "居住地域厳重警戒";
        case 35: return "活火山であることに留意(海底火山)";
        case 36: return "周辺海域警戒";
        case 52: return "噴火";
        case 62: return "噴火したもよう";
        case 127: return "その他の防災気象情報要素";
        default: return "?";
    }
}

// 活動時刻の曖昧さ（Du・3bit）。活動時刻(actDay/actHour/actMin)のうち、どの粒度まで
// 実際に有効な値かを示す（一次資料 Table 4.1.2-30 Parameter Definitions (Volcano) の
// 英文説明から直接訳出）。
const char* dcr_activity_time_ambiguity_name(uint8_t v) {
    switch (v) {
        case 0: return "曖昧さなし";
        case 1: return "概算時刻(分相当)";
        case 2: return "概算時刻(秒)";
        case 3: return "概算時刻(分)：日・時・分が有効";
        case 4: return "概算時刻(時)：日・時が有効、分は無効";
        case 5: return "概算時刻(日)：日が有効、時・分は無効";
        case 6: return "概算時刻(月)";
        case 7: return "概算時刻(年)：日・時・分は無効";
        default: return "?";
    }
}

const char* dcr_ash_fall_warning_code_name(uint8_t v) {
    switch (v) {
        case 1: return "少量の降灰";
        case 2: return "やや多量の降灰";
        case 3: return "多量の降灰";
        case 4: return "小さな噴石の落下";
        case 7: return "その他の防災気象情報要素2";
        default: return "?";
    }
}

// ★level==1「警報解除」が明示的な終了値。
const char* dcr_flood_warning_level_name(uint8_t v) {
    switch (v) {
        case 1:  return "警報解除";
        case 2:  return "氾濫警戒情報";
        case 3:  return "氾濫危険情報";
        case 4:  return "氾濫発生情報";
        case 15: return "その他の警戒レベル";
        default: return "?";
    }
}

const char* dcr_typhoon_reference_time_type_name(uint8_t v) {
    switch (v) {
        case 1: return "実況";
        case 2: return "推定";
        case 3: return "予報";
        default: return "?";
    }
}

const char* dcr_typhoon_scale_category_name(uint8_t v) {
    switch (v) {
        case 0:  return "";           // なし
        case 1:  return "大型";
        case 2:  return "超大型";
        case 15: return "その他の大きさ階級";
        default: return "?";
    }
}

const char* dcr_typhoon_intensity_category_name(uint8_t v) {
    switch (v) {
        case 0:  return "";           // なし
        case 1:  return "強い";
        case 2:  return "非常に強い";
        case 3:  return "猛烈な";
        case 15: return "その他の強さ階級";
        default: return "?";
    }
}

// ★code==0「海上警報解除」が明示的な終了値。
const char* dcr_marine_warning_code_name(uint8_t v) {
    switch (v) {
        case 0:  return "海上警報解除";
        case 10: return "海上着氷警報";
        case 11: return "海上濃霧警報";
        case 12: return "海上うねり警報";
        case 20: return "海上風警報";
        case 21: return "海上強風警報";
        case 22: return "海上暴風警報";
        case 23: return "海上台風警報";
        case 31: return "その他の警報等情報要素";
        default: return "?";
    }
}

// 地方海上予報区（14bit・49件）。一次資料 Table 4.1.2-53 と全件照合済み。
//   表が小さいので他の大きい表と違いここへ直置きする。
struct MarineRegion { uint16_t code; const char* name; };
static const MarineRegion kMarineRegions[] = {
    {1000,"日本海北部及びオホーツク海南部"},{1010,"サハリン東方海上"},{1020,"サハリン西方海上"},{1030,"網走沖"},
    {1040,"宗谷海峡"},{1050,"北海道西方海上"},{1100,"北海道南方及び東方海上"},{1110,"北海道東方海上"},
    {1120,"釧路沖"},{1130,"日高沖"},{1140,"津軽海峡"},{1150,"檜山津軽沖"},
    {2000,"三陸沖"},{2010,"三陸沖東部"},{2020,"三陸沖西部"},{3000,"関東海域"},
    {3010,"関東海域北部"},{3020,"関東海域南部"},{3100,"日本海中部"},{3110,"沿海州南部沖"},
    {3120,"秋田沖"},{3130,"佐渡沖"},{3140,"能登沖"},{3200,"東海海域"},
    {3210,"東海海域東部"},{3220,"東海海域西部"},{3230,"東海海域南部"},{4000,"四国沖及び瀬戸内海"},
    {4010,"瀬戸内海"},{4020,"四国沖北部"},{4030,"四国沖南部"},{4100,"日本海西部"},
    {4110,"日本海北西部"},{4120,"山陰沖東部及び若狭湾付近"},{4130,"山陰沖西部"},{5000,"対馬海峡"},
    {5100,"九州西方海上"},{5110,"済州島西海上"},{5120,"長崎西海上"},{5130,"女島南西海上"},
    {5200,"九州南方海上及び日向灘"},{5210,"日向灘"},{5220,"鹿児島海域"},{5230,"奄美海域"},
    {6000,"沖縄海域"},{6010,"東シナ海南部"},{6020,"沖縄東方海上"},{6030,"沖縄南方海上"},
    {10000,"その他の地方海上予報区"},
};
static const int kMarineRegionCount = sizeof(kMarineRegions) / sizeof(kMarineRegions[0]);

const char* dcr_marine_forecast_region_name(uint16_t code) {
    for (int i = 0; i < kMarineRegionCount; i++)
        if (kMarineRegions[i].code == code) return kMarineRegions[i].name;
    return "?";
}

// 深さ: 9bit。501=500km以深 / 511=不明 / 502〜510 は未定義（仕様上あり得ない）。
const char* dcr_depth_text(uint16_t depthRaw, char* buf, size_t n) {
    if (depthRaw == 501)      snprintf(buf, n, "500km以深");
    else if (depthRaw == 511) snprintf(buf, n, "不明");
    else if (depthRaw > 501)  snprintf(buf, n, "?(%u)", depthRaw);
    else                      snprintf(buf, n, "%ukm", depthRaw);
    return buf;
}

// マグニチュード: 7bit。値は M×10。101=10.0超 / 126=不明(8.0超) / 127=不明。
const char* dcr_magnitude_text(uint8_t magRaw, char* buf, size_t n) {
    if (magRaw == 101)      snprintf(buf, n, "M10.0超");
    else if (magRaw == 126) snprintf(buf, n, "M不明(8.0超)");
    else if (magRaw == 127) snprintf(buf, n, "M不明");
    else if (magRaw > 101)  snprintf(buf, n, "M?(%u)", magRaw);
    else                    snprintf(buf, n, "M%u.%u", magRaw / 10, magRaw % 10);
    return buf;
}
