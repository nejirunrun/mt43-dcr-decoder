// dcr_report.h — QZSS 災危通報（DC Report）デコーダ：純関数API
// =============================================================================
// 【このライブラリでできること】
//   QZSS L1S の 250bit メッセージ（受信機の RXM-SFRBX 等から復元した 32byte）を
//   渡すと、CRC24検証 → 共通部の抽出 → カテゴリ別詳細の抽出、を行う。
//
//     uint8_t msg[32];                       // L1S 250bit（MSB-first パック済み）
//     DcrReport r;
//     if (dcr_decode(msg, r) && r.jmaValid) {
//         if (r.category == DCR_CAT_EEW) { DcrEew e; if (dcr_eew(r, e)) { ... } }
//         if (r.category == DCR_CAT_WEATHER) { DcrWeather w; if (dcr_weather(r, w)) { ... } }
//     }
//
// 【使い方の型（2階建て）】
//   1. dcr_decode() で共通部（DcrReport: 通報区分・カテゴリ・報告時刻・種別）を得る。
//   2. r.category を見て、対応する dcr_xxx(r, out) を呼ぶとカテゴリ別詳細
//      （DcrEew / DcrHypocenter / DcrTsunami 等）が得られる。
//   3. 構造体の数値フィールドはコードのまま入っているものが多いので、
//      表示用の文字列が必要なら対応する dcr_xxx_name() で変換する。
//
// 【API の設計】
//  ・**状態を持たない純関数**。入力=32byte、出力=値。時刻や現在時刻を内部で見ない・
//    グローバル変数を持たない・スレッドセーフ。どこから呼んでも同じ入力には同じ結果を返す。
//  ・**DcrReport は raw[32] を丸ごと保持する**。新しい災害カテゴリに対応する拡張は
//    「DcrXxx 構造体 ＋ dcr_xxx(report, out) 関数」を追加するだけで、DcrReport 自体は
//    変わらない（union で抱き込む設計は取っていない）。
//  ・**再送の同一判定（contentKey）はデコード済みの意味内容から作る**。生バイトや
//    CRC24 は同一内容の再送でも値が変わり得るため指紋には使わない。
//    背景・実測根拠は [DECODER_NOTES.md](../docs/DECODER_NOTES.md) 参照。
//
// 【一次資料】
//   IS-QZSS-DCR-016（内閣府/QZSS）。qzss.go.jp の同意ページ
//   technical/download/is_qzss_dcr_016_agree.html から入手可能。
//   ビット配置・コード表の検証記録は [DECODER_NOTES.md](../docs/DECODER_NOTES.md) 参照。
//
// 【利用条件】⚠ 災危通報の**内容を UI に表示する**場合、内閣府/QZSS の
//   「Additional Terms of Use」により免責事項＋追加利用条件の表示義務がある。
//   デコードのみ（内部処理）であれば発生しない。
//
// 【関連ファイル】
//   dcr_report.cpp : 本ヘッダの実装（値テーブルもここ）
//   docs/DECODER_NOTES.md : 設計判断・実測で踏んだ落とし穴（開発経緯）
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>

// ── 災害カテゴリ（disaster_category・4bit）──────────────────────────────────
//   MT43（JMA 本報）が内部で 14 種を切り替えるためのコード。詳細部のビット配置は
//   カテゴリごとに完全に別物なので、必ずこの値で分岐してから詳細デコーダを呼ぶ。
enum DcrCategory : uint8_t {
    DCR_CAT_EEW              = 1,   // 緊急地震速報   → dcr_eew()
    DCR_CAT_HYPOCENTER       = 2,   // 震源           → dcr_hypocenter()（★実座標あり）
    DCR_CAT_SEISMIC          = 3,   // 震度           （〃）
    DCR_CAT_NANKAI           = 4,   // 南海トラフ地震 （〃）
    DCR_CAT_TSUNAMI          = 5,   // 津波           （〃）
    DCR_CAT_NWPAC_TSUNAMI    = 6,   // 北西太平洋津波 （〃）
    DCR_CAT_VOLCANO          = 8,   // 火山           （〃）
    DCR_CAT_ASHFALL          = 9,   // 降灰           （〃）
    DCR_CAT_WEATHER          = 10,  // 気象           → dcr_weather()
    DCR_CAT_FLOOD            = 11,  // 洪水           → dcr_flood()
    DCR_CAT_TYPHOON          = 12,  // 台風           （〃）
    DCR_CAT_MARINE           = 14,  // 海上         （〃）
};

// ── 共通部（すべてのカテゴリで同じ位置にある情報）────────────────────────────
//   MT44（DCX＝他機関）は JMA とまったく別フォーマットなので、mt==44 のとき
//   classification 以降の JMA フィールドは**意味を持たない**（jmaValid で判別）。
struct DcrReport {
    bool     valid;          // CRC24 一致かつ MT43/44 かつ PAB 正当
    bool     jmaValid;       // mt==43（JMA 本報）＝以下の共通部フィールドが有効
    uint8_t  mt;             // 43=JMA 本報 / 44=DCX（他機関。共通部より先は未デコード）
    // Rc 3bit: 通報区分（JMA自身による緊急度の判断）。
    //   1=最優先 2=優先 3=通常 7=訓練/試験（0,4,5,6 は仕様上未割当）。
    //   最優先は通常より高頻度で再送される。訓練/試験(7)は優先度の序列とは別軸のマーカー。
    uint8_t  classification;
    uint8_t  category;       // Dc 4bit（DcrCategory）。この値で分岐して詳細デコーダを呼ぶ
    // At 報告時刻。**UTC**（JSTで使うときは dcr_to_jst() で変換する）。
    uint8_t  month, day;
    uint8_t  hour, minute;
    // It 2bit: 0=発表 / 1=訂正 / 2=取消（dcr_info_type_name）。
    uint8_t  infoType;
    // 再送の同一判定キー。デコード済みの意味内容から作られる（生バイトやCRC24は含まない）。
    //   同一通報の再送 → 同じ値／別の通報 → 別の値。通知の identity にそのまま使える。
    //   詳細デコーダが無いカテゴリは共通部のみで作るため識別粒度が粗くなる
    //   （詳細デコーダがあるカテゴリはそのフィールドも材料に含める）。
    uint32_t contentKey;
    uint8_t  raw[32];        // 250bit 原文。カテゴリ別詳細デコーダはここから都度読む
};

// 32byte の L1S メッセージを検証・共通部デコードする。
//   @return valid と同値。false なら out.valid=false 以外の中身は未定義。
//   CRC 不一致・PAB 不正・MT43/44 以外は false（＝呼び出し側は捨ててよい）。
bool dcr_decode(const uint8_t msg[32], DcrReport& out);

// ── 緊急地震速報（category==DCR_CAT_EEW）詳細 ────────────────────────────────
//   座標を持たない。「どこが揺れるか」を対象地域コードで、震度は上下限の予測値で
//   伝える（観測震度ではない）。地図に置きたい場合は震源(DCR_CAT_HYPOCENTER)を待つ。
//   「仮定震源要素」(assumptive): 深さ raw==10km かつ マグニチュード raw==10(=M1.0)
//     のとき、JMA は震源を確定せず PLUM 法等で震度のみ推定している。この場合、
//     震源・規模の値を額面どおり表示してはいけない（実際の地震は M1.0 ではない）。
struct DcrEew {
    uint8_t  lgLower, lgUpper;   // 長周期地震動階級 下限/上限（3bit・0=設定なし）
    uint16_t prevention[3];      // 防災上の留意事項コード 9bit×3（0=以降なし）
    uint8_t  otDay, otHour, otMin; // 地震発生時刻（報告時刻とは別）
    uint16_t depthRaw;           // 9bit: km / 501=500km以深 / 511=不明
    uint8_t  magRaw;             // 7bit: M×10 / 101=10.0超 / 126,127=不明
    uint16_t epicenterCode;      // 10bit 震央地名コード（dcr_epicenter_name）
    uint8_t  intensityLower;     // 4bit 震度下限（dcr_intensity_name）
    uint8_t  intensityUpper;     // 4bit 震度上限（〃・11=「〜程度以上」）
    bool     assumptive;         // 仮定震源要素（上記★）
    uint8_t  regionCount;        // 対象地域の数
    uint8_t  regionBits[10];     // 80bit ビットマップ（地域コード n は bit n-1）。
                                 //   ロスレス保持。判定は dcr_eew_has_region() を使う。
};
bool dcr_eew(const DcrReport& r, DcrEew& out);

// 地域コード（1〜80）が対象に含まれるか。地域フィルタ（設定アプリ連携）の土台。
bool dcr_eew_has_region(const DcrEew& e, uint8_t regionCode);

// ── 震源（category==DCR_CAT_HYPOCENTER）詳細 ─────────────────────────────────
//   実座標を積んでいるのはこのカテゴリ（EEWは座標を持たない）。「どこで起きたか」を
//   度分秒＋十進度で伝える確定情報。
//
//   EEW とはペア配信ではない点に注意（気象庁の発表基準が別）:
//     EEW  … 予測が震度5弱以上 または 長周期地震動階級3以上 → 検知の数秒後に配信
//     震源 … 観測が震度3以上 かつ 津波の心配なし           → 数分後に配信
//   震度3〜4程度の地震は震源だけが来てEEWは来ない（大多数のケース）。逆に大地震では
//   EEWが先に来て、震源はその後の確定情報として遅れて届く。UI側は「EEWは座標なしで
//   完結させ、地図ピンは震源が来てから置く」という非同期性を前提に設計すること。
struct DcrHypocenter {
    uint16_t prevention[3];        // 防災上の留意事項コード（0=以降なし）
    uint8_t  otDay, otHour, otMin; // 地震発生時刻
    uint16_t depthRaw;             // 9bit: km / 501=500km以深 / 511=不明
    uint8_t  magRaw;               // 7bit: M×10 / 101=10.0超 / 126,127=不明
    uint16_t epicenterCode;        // 10bit 震央地名コード
    // 生の度分秒（仕様どおりの整数値。表示で「北緯33度2分…」と出したいとき用）
    bool     latSouth;             // false=北緯 / true=南緯（生 bit の 0/1）
    uint8_t  latDeg, latMin, latSec;
    bool     lonWest;              // false=東経 / true=西経
    uint8_t  lonDeg, lonMin, lonSec;
    // 十進度（南緯・西経は負）。マップへのプロット・自位置からの距離計算はこちらを使う。
    double   latitude, longitude;
};
bool dcr_hypocenter(const DcrReport& r, DcrHypocenter& out);

// ── 震度（category==DCR_CAT_SEISMIC）詳細 ────────────────────────────────────
//   「どこで震度いくつを観測したか」。EEW（予測）と違い**観測された事実**を伝える。
//   都道府県ごとに最大16件。★震度コードは 3bit で、EEW の 4bit 表とは**別物**
//   （dcr_seismic_intensity_name / dcr_intensity_name を取り違えないこと）。
struct DcrSeismic {
    uint8_t otDay, otHour, otMin;  // 地震発生時刻（UTC）
    uint8_t count;                 // 有効な (震度, 都道府県) 組の数（最大16）
    uint8_t intensity[16];         // 震度コード（3bit・dcr_seismic_intensity_name）
    uint8_t prefecture[16];        // 都道府県コード（6bit・dcr_prefecture_name）
};
bool dcr_seismic(const DcrReport& r, DcrSeismic& out);

// ── 津波（category==DCR_CAT_TSUNAMI）詳細 ────────────────────────────────────
//   国内向けの津波警報。津波予報区ごとに最大5件。
//   ★到達予想時刻の「時」31 / 「分」63 は不明を表す（そのまま保持し、表示側で判断）。
struct DcrTsunami {
    uint16_t prevention[3];   // 防災上の留意事項コード（0=以降なし）
    uint8_t  warningCode;     // 4bit: 1=津波なし 2=警報解除 3=津波警報 4/5=大津波警報
    uint8_t  count;           // 有効な組の数（最大5）
    uint8_t  nextDay[5];      // 到達予想が翌日なら 1（報告日からのオフセット）
    uint8_t  arrHour[5];      // 到達予想 時（31=不明）
    uint8_t  arrMin[5];       // 到達予想 分（63=不明）
    uint8_t  height[5];       // 津波の高さコード（4bit・dcr_tsunami_height_name）
    uint16_t region[5];       // 津波予報区コード（10bit・dcr_tsunami_region_name）
};
bool dcr_tsunami(const DcrReport& r, DcrTsunami& out);

// ── 気象（category==DCR_CAT_WEATHER）詳細 ────────────────────────────────────
struct DcrWeather {
    uint8_t  warningState;   // 1=発表 2=解除
    uint8_t  count;          // 有効な (sub, region) 組の数（最大6）
    uint8_t  sub[6];         // 警報等情報要素コード（5bit）
    uint32_t region[6];      // 府県予報区コード（19bit）
};
bool dcr_weather(const DcrReport& r, DcrWeather& out);

// ── 南海トラフ地震関連情報（category==DCR_CAT_NANKAI）詳細 ───────────────────
//   他カテゴリと違い、地域も座標も持たない「テキスト速報」。
//   text[18] はUTF-8文字列の1ページ分の生バイト。一次資料上、長い文字列は複数
//   メッセージ（ページ）に分割して送信される。1メッセージ分の18byteだけでは
//   文字境界が保証されないため、複数ページを跨いだ結合はこのデコーダの責務外
//   （呼び出し側で pageNumber/totalPage を見て結合すること）。
struct DcrNankai {
    uint8_t infoSerialCode;   // 4bit: 1〜3=調査中A/B/C 4=巨大地震警戒 5=注意 6=調査終了 15=その他
    uint8_t text[18];         // 見出しテキストの1ページ分（UTF-8・生バイト・8bit×18）
    uint8_t pageNumber;       // 6bit: このメッセージが何ページ目か（1始まり）
    uint8_t totalPage;        // 6bit: 総ページ数
};
bool dcr_nankai(const DcrReport& r, DcrNankai& out);

// ── 北西太平洋津波（category==DCR_CAT_NWPAC_TSUNAMI）詳細 ────────────────────
//   国際向け（カムチャツカ・沿海州・台湾等）。地名コードの名称表は英語名のみ。
struct DcrNwPacTsunami {
    uint8_t  potential;         // 3bit: 0=可能性なし 1〜4=規模別 7=その他
    uint8_t  count;             // 有効件数（最大5）
    bool     nextDay[5];        // 到達予想が翌日か
    uint8_t  arrHour[5];        // 到達予想 時（31=不明）
    uint8_t  arrMin[5];         // 到達予想 分（63=不明）
    uint16_t heightRaw[5];      // 9bit: 1〜4=規模別 508=10m超 509=巨大 510=高い 511=不明
    uint8_t  region[5];         // 7bit 沿岸地域コード（英語名のみ・dcr_coastal_region_name）
};
bool dcr_nwpac_tsunami(const DcrReport& r, DcrNwPacTsunami& out);

// ── 火山（category==DCR_CAT_VOLCANO）詳細 ────────────────────────────────────
//   噴火警戒レベル・警戒範囲。同じ火山名で継続的に更新される想定の情報で、
//   レベルの終了を示す専用コードは無い（レベルが下がって「平常」相当に戻る形で表現される）。
struct DcrVolcano {
    uint8_t  ambiguityOfActivityTime;  // 3bit: 活動時刻(actDay/actHour/actMin)のうち
                                       //   どこまでが有効かを示す（dcr_activity_time_ambiguity_name）
    uint8_t  actDay, actHour, actMin;  // 活動時刻
    uint8_t  warningCodeRaw;           // 7bit: 11〜15=レベル1〜5 21〜25/35/36/52/62=状態表現 127=その他
    uint16_t volcanoNameRaw;           // 12bit（dcr_volcano_name）
    uint8_t  localGovCount;            // 有効な市町村数（最大5）
    uint32_t localGov[5];              // 23bit 市町村コード（dcr_local_government_name）
};
bool dcr_volcano(const DcrReport& r, DcrVolcano& out);

// ── 降灰（category==DCR_CAT_ASHFALL）詳細 ────────────────────────────────────
//   降灰予報。市町村ごとに「いつ頃・どの程度」を最大4件持つ。終了コードは無く、
//   活動が続く限り同じ火山名で更新され続ける情報。
struct DcrAshFall {
    uint8_t  actDay, actHour, actMin;   // 活動時刻
    uint8_t  warningType;               // 2bit: 1=速報 2=詳細
    uint16_t volcanoNameRaw;            // 12bit（dcr_volcano_name と共用の表）
    uint8_t  count;                     // 有効件数（最大4）
    // 3bit: コード表ではなく活動時刻(actDay/actHour/actMin)からの経過時間（単位=時）そのもの。
    //   名前解決は不要で「Ho時間後」とそのまま表示できる値。
    uint8_t  expectedTime[4];
    uint8_t  warningCodeRaw[4];         // 3bit（dcr_ash_fall_warning_code_name）
    uint32_t localGov[4];               // 23bit 市町村コード（dcr_local_government_name と共用）
};
bool dcr_ash_fall(const DcrReport& r, DcrAshFall& out);

// ── 洪水（category==DCR_CAT_FLOOD）詳細 ──────────────────────────────────────
//   ★警戒レベル 1=「警報解除」が明示的な終了値（継続中/解除済みの判定に使える）。
//   予報地域コードは40bit（河川ごとの細かいコード体系）で uint32_t に収まらない
//   ため uint64_t で持つ。
struct DcrFlood {
    uint8_t  count;          // 有効件数（最大3）
    uint8_t  level[3];       // 4bit: 1=警報解除 2=氾濫警戒 3=氾濫危険 4=氾濫発生 15=その他
    uint64_t region[3];      // 40bit 洪水予報区（dcr_flood_forecast_region_name）
};
bool dcr_flood(const DcrReport& r, DcrFlood& out);

// ── 台風（category==DCR_CAT_TYPHOON）詳細 ────────────────────────────────────
//   座標を持つカテゴリ（震源以外で唯一）。終了コードは無く、台風が消滅・
//   温帯低気圧化するまで継続的に更新される情報。同一性キーは台風番号
//   （1〜99・シーズン内でユニーク）。
struct DcrTyphoon {
    uint8_t  refDay, refHour, refMin;  // 解析基点時刻
    uint8_t  refTimeType;              // 3bit: 1=実況 2=推定 3=予報
    uint8_t  elapsedFromRef;           // 7bit 基点からの経過時間
    uint8_t  typhoonNumber;            // 7bit（1〜99号）
    uint8_t  scaleCategory;            // 4bit: 0=なし 1=大型 2=超大型 15=その他
    uint8_t  intensityCategory;        // 4bit: 0=なし 1=強い 2=非常に強い 3=猛烈な 15=その他
    // 度分秒（震源と同じ形式）。十進度は latitude/longitude。
    bool     latSouth; uint8_t latDeg, latMin, latSec;
    bool     lonWest;  uint8_t lonDeg, lonMin, lonSec;
    double   latitude, longitude;
    uint16_t centralPressureRaw;       // 11bit hPa（最大1100）
    uint8_t  maxWindSpeedRaw;          // 7bit m/s（0=不明・15〜105）
    uint8_t  maxGustWindSpeedRaw;      // 7bit m/s（0=不明・15〜105）
};
bool dcr_typhoon(const DcrReport& r, DcrTyphoon& out);

// ── 海上（category==DCR_CAT_MARINE）詳細 ───────────────────────────────────
//   地方海上予報区ごとの高波・強風等の警報。警報コード 0 が「解除」の明示的な値。
struct DcrMarine {
    uint8_t  count;          // 有効件数（最大8）
    uint8_t  code[8];        // 5bit: 0=解除 10〜23=警報種別 31=その他（dcr_marine_warning_code_name）
    uint16_t region[8];      // 14bit 地方海上予報区（dcr_marine_forecast_region_name）
};
bool dcr_marine(const DcrReport& r, DcrMarine& out);

// ── コード → 名称（表示層・シリアル出力の共用）──────────────────────────────
//   未定義コードは "?" を返す（nullptr は返さない＝呼び出し側で null チェック不要）。
const char* dcr_classification_name(uint8_t rc);
const char* dcr_category_name(uint8_t dc);
const char* dcr_info_type_name(uint8_t it);         // 0=発表 1=訂正 2=取消
const char* dcr_intensity_name(uint8_t v);          // 震度 4bit（EEW の予想震度・上限の 11 含む）
// 震度カテゴリ(3)の観測震度は **3bit の別表**。上の 4bit 表（EEW予想震度）とは値が別。
const char* dcr_seismic_intensity_name(uint8_t v);  // 震度 3bit（観測震度）
const char* dcr_prefecture_name(uint8_t code);      // 都道府県 6bit（47件）
const char* dcr_tsunami_warning_name(uint8_t v);    // 津波警報コード 4bit
const char* dcr_tsunami_height_name(uint8_t v);     // 津波の高さ 4bit
const char* dcr_tsunami_region_name(uint16_t code); // 津波予報区 10bit（99件）
const char* dcr_long_period_name(uint8_t v);        // 長周期地震動階級
const char* dcr_eew_region_name(uint8_t code);      // EEW 対象地域（1〜80・80bitビットマップ用）
// 震央地名（10bit・EEW と震源が共通で持つ「どこの地震か」）。EEWは座標を持たないので、
//   EEWにおける地名解決はこの関数が唯一の手段になる。表は dcr_epicenter_table.cpp。
const char* dcr_epicenter_name(uint16_t code);
// 防災上の留意事項（9bit・EEW/震源/津波/北西太平洋津波が 3 個ずつ持つ）。
//   表は dcr_prevention_table.cpp（一次資料 Table 4.1.2-6・全 55 件）。
//   この関数だけ未知コードで nullptr を返す（他は "?"）。仕様改版で新コードが送られ
//   得るため、呼び出し側は数値のフォールバック表示を用意し null チェックすること。
const char* dcr_prevention_name(uint16_t code);
const char* dcr_weather_state_name(uint8_t v);
const char* dcr_weather_sub_name(uint8_t v);
const char* dcr_weather_region_name(uint32_t code); // 府県予報区（75件）

// ── 残り7カテゴリの名称テーブル ────────────────────────────────────────────
const char* dcr_nankai_serial_code_name(uint8_t v);         // 南海トラフ情報種類（4bit）
const char* dcr_tsunamigenic_potential_name(uint8_t v);     // 北西太平洋津波 発生可能性（3bit）
const char* dcr_nwpac_tsunami_height_name(uint16_t v);      // 北西太平洋津波 高さ（9bit）
const char* dcr_coastal_region_name(uint8_t code);          // 沿岸地域（7bit・英語名のみ・98件）
const char* dcr_volcanic_warning_code_name(uint8_t v);      // 火山警戒コード（7bit）
const char* dcr_activity_time_ambiguity_name(uint8_t v);    // 活動時刻の曖昧さ（Du・3bit）
const char* dcr_volcano_name(uint16_t code);                // 火山名（12bit・121件）
const char* dcr_local_government_name(uint32_t code);       // 市町村（23bit・1792件）
const char* dcr_ash_fall_warning_code_name(uint8_t v);      // 降灰警報コード（3bit）
const char* dcr_flood_warning_level_name(uint8_t v);        // 洪水警戒レベル（4bit）
const char* dcr_flood_forecast_region_name(uint64_t code);  // 洪水予報区（40bit・392件）
const char* dcr_typhoon_reference_time_type_name(uint8_t v);// 台風 基点時刻種別（3bit）
const char* dcr_typhoon_scale_category_name(uint8_t v);     // 台風 大きさ階級（4bit）
const char* dcr_typhoon_intensity_category_name(uint8_t v); // 台風 強さ階級（4bit）
const char* dcr_marine_warning_code_name(uint8_t v);        // 海上警報コード（5bit）
const char* dcr_marine_forecast_region_name(uint16_t code); // 地方海上予報区（14bit・49件）

// 通報1件の人間可読な1行要約（時刻は JST へ変換済み）。ログ・シリアル出力・簡易表示に
//   共通して使える。詳細デコーダのあるカテゴリは中身入り、無いカテゴリはカテゴリ名＋発令時刻。
//   例: "震源 20:47 熊本県熊本地方 10km M4.4 (32.6000,130.7000)"
void dcr_summary(const DcrReport& r, char* buf, size_t n);

// 深さ・マグニチュードは特殊コードがあるため文字列化を用意する（buf へ書いて返す）。
//   例: "深さ10km" / "500km以深" / "不明"、"M6.5" / "M10.0超" / "M不明"
const char* dcr_depth_text(uint16_t depthRaw, char* buf, size_t n);
const char* dcr_magnitude_text(uint8_t magRaw, char* buf, size_t n);

// ── 時刻の扱い ──────────────────────────────────────────────────────────────
// 電文の時刻は **UTC**。日本の利用者へ出すときは JST（UTC+9）へ直す必要がある。
//   月日をまたぐので単純な加算では足りない（23:30 UTC → 翌日 08:30 JST）。
//   引数は in/out。month は日跨ぎ・月跨ぎの判定に使う（不要なら nullptr 可だが、
//   その場合 day は 1〜31 の循環になり月末で誤る）。
//   地震発生時刻（DcrEew/DcrHypocenter の otDay/otHour/otMin）にも月が無いので、
//   同じ通報の報告時刻の月（r.month）を渡して変換すること。
//   既知の限界: 電文に年が無いため閏年を判定できない。2月28日 15:00 UTC 以降だけ
//   日付が1日ずれ得る（平年として扱う）。実用上は許容範囲と判断。
void dcr_to_jst(uint8_t* month, uint8_t* day, uint8_t* hour, uint8_t* minute);

// 内容キーを作る（dcr_decode が内部で呼び r.contentKey に入れる）。単体でも使える。
//   材料 = 共通部（MT/カテゴリ/報告時刻/情報種別）＋ 詳細デコーダが有るカテゴリは
//   その全フィールド。生バイトやCRC24は混ぜない（背景は docs/DECODER_NOTES.md）。
uint32_t dcr_content_key(const DcrReport& r);

// ── 低レベルユーティリティ（受信経路が PAB/MT の速判定に使う）───────────────
uint32_t dcr_bits(const uint8_t* msg, int bitOffset, int bitLen);  // MSB-first ビット読み
bool     dcr_pab_valid(uint8_t pab);   // プリアンブル 3 値の巡回（IS-QZSS-L1S §4.1.2.2）
