// dcr_report.h — QZSS 災危通報（DC Report）デコーダ：純関数レイヤ（L1）
// =============================================================================
// 【このファイルで何ができるか】
//   QZSS L1S の 250bit メッセージ（RXM-SFRBX から復元した 32byte）を受け取り、
//   CRC24 検証 → 共通部の抽出 → カテゴリ別詳細の抽出、までを **純関数** で行う。
//
//     uint8_t msg[32];                       // L1S 250bit（MSB-first パック済み）
//     DcrReport r;
//     if (dcr_decode(msg, r) && r.mt == 43) {
//         if (r.category == DCR_CAT_EEW) { DcrEew e; if (dcr_eew(r, e)) { ... } }
//         if (r.category == DCR_CAT_WEATHER) { DcrWeather w; if (dcr_weather(r, w)) { ... } }
//     }
//
// 【設計判断】
//  ・**状態を持たない**。時刻も millis() も見ない。入力=32byte、出力=値。
//    受信経路（GPSControl の診断ループ／本番 gpsUpdate）と、保管（notice_store）と、
//    表示（display 通知センター）から完全に独立させる＝どこから呼んでも同じ結果になり、
//    実機なしで検証できる。テレメトリ心臓部へは一切書かない（保全の不変条件）。
//
//  ・**DcrReport は raw[32] を丸ごと抱える**。これが「ゴタゴタしても壊れない」ための核。
//    新しい災害カテゴリ（津波・火山・洪水…）に対応するとき、追加するのは
//    「DcrXxx 構造体 ＋ dcr_xxx(report, out) 関数」だけで、**DcrReport 自体は変わらない**。
//    共用体（union）で詳細を抱き込む形にすると、カテゴリを足すたびに構造体レイアウトが
//    動き、保管層（リング）や表示層まで巻き添えで壊れる。それを構造的に避けている。
//    ★CLAUDE.md「struct にメンバを追加したらクリーンビルド」の地雷を、そもそも踏まない設計。
//
//  ・**重複判定は「デコード済みの意味内容」から作る**（contentKey）。生バイトや CRC24 を
//    指紋にしてはいけない。理由は2段階で判明した:
//      ① PAB（プリアンブル）は内容と無関係に巡回し、CRC24 の計算範囲 bit0〜225 の
//         先頭に含まれる → 素の CRC24 は同一内容でも変わる（2026-07-27 実測）。
//      ② PAB を除いてもなお、同一内容の MT43 が再送のたびに別値になった（2026-07-28 実測。
//         熊本の余震の同一通報が3回とも別値）。**MT43 には確定した区切り・長さ・順番が
//         無く、観測側からその構造を知ることはできない**。「同じ情報がカルーセルして
//         いるように見える」だけで、ビット列の同一性は保証されていない。
//    → よって「どのビットをマスクすれば安定するか」を探す方向は原理的に袋小路。
//      デコードして意味が取れたフィールドだけを材料に鍵を作る（＝下の dcr_content_key）。
//      これは表示・通知の identity としても正しい粒度になる（人間が「同じ通報だ」と
//      判断する材料と一致するため）。
//
// 【資料の格付け】★2026-07-30 明確化（ユーザー指摘）
//   ・**一次資料 = IS-QZSS-DCR-016（内閣府/QSS）**。これだけが規範。
//     入手経路: qzss.go.jp の同意ページ technical/download/is_qzss_dcr_016_agree.html
//     → PDF 直リンク（en/technical/download/pdf/ps-is-qzss/is-qzss-dcr-016.pdf）
//     併せて「訓練／試験メッセージ 配信情報の詳細」（月次PDF）も一次資料。
//   ・**azarashi（MIT・nbtk 氏）は第三者実装＝傍証**であって一次資料ではない。
//     ビット配置の突き合わせ相手として有用だが、食い違ったら一次資料が勝つ。
//     ★旧コメントは「azarashi と照合済み＝検証済み」と読める書き方をしていたが、
//       これは資料の格付けを誤った表現だったので改めた。実際 Rc の対応表は
//       azarashi が正しく我々が誤っていた（README 落とし穴⑤）。
//   EEW のビット配置は azarashi qzss_dcr_decoder_jma_earthquake_early_warning.py と照合
//   （2026-07-28）。47/50/53/80/96/105/112/122/126/130 が隙間なく連続することを確認済み。
//
// 【利用条件】⚠ 災危通報の**内容を UI に表示する**場合、内閣府/QSS の
//   「Additional Terms of Use」により免責事項＋追加利用条件の表示義務がある。
//   デコードだけなら発生しないが、通知センターへ載せる段階（B5）で必ず対応すること。
//
// 【関連ファイル】
//   dcr_report.cpp                 : 本ヘッダの実装（値テーブルもここ）
//   ../GPSControl.cpp dumpDcrLoop(): 診断用の受信ループ（本モジュールの最初の消費者）
//   ../../../notice_store/         : L2 通知内容ストア（B2 で新設予定・未実装）
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
    uint8_t  mt;             // 43=JMA 本報 / 44=DCX（他機関）
    // Rc 3bit: 1=最優先 2=優先 3=通常 7=訓練/試験（0,4,5,6 は仕様上未割当）
    //   ★JMA 自身による緊急度の判断であり、捨ててはいけない重要情報
    //     （最優先は通常の20倍の頻度で再送される＝IS-QZSS-DCR-016 Table 4.1.1-1）。
    //     訓練/試験(7)は優先度の序列の外にある別軸のマーカー。詳細は
    //     dcr_report.cpp の dcr_classification_name() 冒頭コメント（1つズレ事故の碑文）。
    uint8_t  classification;
    uint8_t  category;       // Dc 4bit（DcrCategory）
    // At 報告時刻。★**UTC で入っている**（実測で確定・2026-07-28: 受信した全通報が
    //   例外なく日本時間から9時間遅れており、14:06 の通報を 23:06 JST に受信した）。
    //   ここは電文に忠実な生の値を保持し、表示のときだけ dcr_to_jst() で変換する
    //   （受信直後に足し込むと、生データと突き合わせたときに合わなくなるため）。
    uint8_t  month, day;
    uint8_t  hour, minute;
    // It 2bit: 0=発表 / 1=訂正 / 2=取消（dcr_info_type_name）。
    //   ★「取消」が存在することが、DC Report が**イベント列ではなく「いま有効な通報の
    //     掲示板」**であることの根拠になっている（発表から取消まで載り続ける）。
    //     詳細は README.md「掲示板モデル」。
    uint8_t  infoType;
    // 再送の同一判定キー。**デコード済みの意味内容だけ**から作る（設計判断②を参照）。
    //   同一通報の再送 → 同じ値／別の通報 → 別の値。通知の identity にそのまま使える。
    //   ※詳細デコーダが無いカテゴリは共通部（MT・カテゴリ・報告時刻・種別）のみで作るため
    //     粒度が粗い＝「同じカテゴリ・同じ報告時刻の別内容」を取り違える理論上の余地が残る。
    //     カテゴリの詳細デコーダを実装すると自動的に精度が上がる（dcr_content_key を参照）。
    uint32_t contentKey;
    uint8_t  raw[32];        // 250bit 原文。カテゴリ別詳細はここから都度読む（設計判断参照）
};

// 32byte の L1S メッセージを検証・共通部デコードする。
//   @return valid と同値。false なら out.valid=false 以外の中身は未定義。
//   CRC 不一致・PAB 不正・MT43/44 以外は false（＝呼び出し側は捨ててよい）。
bool dcr_decode(const uint8_t msg[32], DcrReport& out);

// ── 緊急地震速報（category==DCR_CAT_EEW）詳細 ────────────────────────────────
//   ★「仮定震源要素」(assumptive): 深さ raw==10km かつ マグニチュード raw==10(=M1.0)
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
//   ★**実座標を積んでいるのはこのカテゴリ**（緊急地震速報ではない・2026-07-28 確認）。
//     EEW(1) と震源(2) は bit121 まで同一レイアウトで、そこから分岐する:
//       EEW  … +122 予想震度(上下限) → +130 対象地域80bitビットマップ
//       震源 … +122 度分秒の実座標 41bit（緯度 1+7+6+6 ／ 経度 1+8+6+6）
//     つまり EEW は「どこが揺れるか」を地域コードで、震源は「どこで起きたか」を
//     座標で伝える。用途が違うので両方要る（EEW＝即時警報／震源＝確定情報）。
//     座標があるためマップへのプロットが可能（将来の route/map 連携の素材）。
//
//   ★**両者はペア配信ではない**（気象庁の発表基準が別・2026-07-28 確認）:
//       EEW(1)  … 予測が震度5弱以上 または 長周期地震動階級3以上 → 検知の数秒後
//       震源(2) … 観測が震度3以上 かつ 津波の心配なし           → 数分後
//     ＝震度3〜4 の地震は**震源(2)だけ**が来て EEW は来ない（大多数のケース）。
//     大地震では EEW が先、震源が後から追いつく。よって UI 設計では
//       ・EEW は座標が無い前提で「地域名＋予想震度」だけで完結させる（地図ピンを待たない）
//       ・地図ピンは震源(2)が来て初めて置ける＝EEW の続報として遅れて現れる
//     という非対称を織り込むこと。EEW に座標が来ないのは欠落ではなく設計である。
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

// =============================================================================
// 【2026-07-30 追加：残り7カテゴリの詳細デコーダ】
//   azarashi（MIT・nbtk氏）の該当 decoder/*.py を実機のソースで直接確認し
//   （要約AIを介さず生コードを読んだ）、ビット配置を転記した。掲示板モデル上の
//   分類（状態／履歴）は README「掲示板モデル」の判定基準（専用フィールドに
//   明示的な終了値があるか）で導いたが、台風・火山・降灰は終了値が無いにも
//   関わらずユーザー判断で「状態」扱いとした（時間的継続を伴う中間的イベント
//   という実情を優先・2026-07-30 合意）。dcr_board.cpp 側で全カテゴリ共通の
//   実測間隔ベースタイムアウトが降板の唯一の手段になる。
// =============================================================================

// ── 南海トラフ地震関連情報（category==DCR_CAT_NANKAI）詳細 ───────────────────
//   ★他カテゴリと違い、地域も座標も持たない「テキスト速報」。情報種類コードが
//     6=調査終了で明示的に終わる＝状態バケツに分類する根拠（README参照）。
//   text[18] は生バイトのまま持つ。★一次資料 Table 4.1.2-18 に「UTF-8 文字列を
//     複数ページに分割して送信する」と明記されている（2026-07-30 確認・落とし穴⑧の
//     解消）。1メッセージ分の18byteだけでは文字境界が保証されないので、このデコーダ
//     の責務はここまで＝**複数ページを跨いだ結合は dcr_nankai_track が正本**
//     （台風の実況/予報レグと同じ理由でデコーダ層に持たせない）。
struct DcrNankai {
    uint8_t infoSerialCode;   // 4bit: 1〜3=調査中A/B/C 4=巨大地震警戒 5=注意 6=調査終了 15=その他
    uint8_t text[18];         // 見出しテキストの1ページ分（UTF-8・生バイト・8bit×18）
    uint8_t pageNumber;       // 6bit: このメッセージが何ページ目か（1始まり）
    uint8_t totalPage;        // 6bit: 総ページ数
};
bool dcr_nankai(const DcrReport& r, DcrNankai& out);

// ── 北西太平洋津波（category==DCR_CAT_NWPAC_TSUNAMI）詳細 ────────────────────
//   国際向け（カムチャツカ・沿海州・台湾等）。地名は国内向けと違い azarashi も
//   英語名のみ（`qzss_dcr_jma_coastal_region.py` に日本語版が存在しない）。
//   ★発生可能性 0=「津波の可能性なし」が明示的な終了値＝国内津波と同じ状態バケツ。
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
//   噴火警戒レベル・警戒範囲。★終了を表す専用コードは無いが、レベルが下がって
//   「平常」相当に戻ることも同じキー（火山名）の状態更新として自然に表現できるため
//   状態バケツとして扱う（2026-07-30 ユーザー判断）。
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
//   降灰予報。市町村ごとに「いつ頃・どの程度」を最大4件持つ。★終了コードは無いが
//   火山と同様の理由で状態バケツ扱い（活動が続く限り同じ火山名で更新され続ける）。
struct DcrAshFall {
    uint8_t  actDay, actHour, actMin;   // 活動時刻
    uint8_t  warningType;               // 2bit: 1=速報 2=詳細
    uint16_t volcanoNameRaw;            // 12bit（dcr_volcano_name と共用の表）
    uint8_t  count;                     // 有効件数（最大4）
    // 3bit: コード表ではなく**活動時刻(actDay/actHour/actMin)からの経過時間（単位=時）**
    //   そのもの（一次資料 Ho・Effective Range 0/1-6）。名前解決は不要＝「Ho時間後」と
    //   そのまま表示すればよい値（★以前「詳細未解読」としていたのは表と早合点した誤り）。
    uint8_t  expectedTime[4];
    uint8_t  warningCodeRaw[4];         // 3bit（dcr_ash_fall_warning_code_name）
    uint32_t localGov[4];               // 23bit 市町村コード（dcr_local_government_name と共用）
};
bool dcr_ash_fall(const DcrReport& r, DcrAshFall& out);

// ── 洪水（category==DCR_CAT_FLOOD）詳細 ──────────────────────────────────────
//   ★警戒レベル 1=「警報解除」が明示的な終了値＝気象と同型の状態バケツ。
//   予報地域コードは40bit（河川ごとの細かいコード体系）で uint32_t に収まらない
//   ため uint64_t で持つ。
struct DcrFlood {
    uint8_t  count;          // 有効件数（最大3）
    uint8_t  level[3];       // 4bit: 1=警報解除 2=氾濫警戒 3=氾濫危険 4=氾濫発生 15=その他
    uint64_t region[3];      // 40bit 洪水予報区（dcr_flood_forecast_region_name）
};
bool dcr_flood(const DcrReport& r, DcrFlood& out);

// ── 台風（category==DCR_CAT_TYPHOON）詳細 ────────────────────────────────────
//   座標を持つカテゴリ（震源・南海トラフ以外で唯一）。★終了コードは無いが、
//   台風が消滅・温帯低気圧化するまで「継続する状況」であり、震源のような一発
//   イベントではないため状態バケツ扱い（2026-07-30 ユーザー判断）。
//   同一性キーは台風番号（1〜99・シーズン内でユニーク）。
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
//   ★警報コード 0=「海上警報解除」が明示的な終了値＝気象と同型の状態バケツ。
//   沿岸ドライバー・釣り・マリンスポーツ利用者にとって重要度の高い高波・強風情報。
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
// ★震度カテゴリ(3)の震度は **3bit の別表**。上の 4bit 表と取り違えると別の震度が出る。
const char* dcr_seismic_intensity_name(uint8_t v);  // 震度 3bit（観測震度）
const char* dcr_prefecture_name(uint8_t code);      // 都道府県 6bit（47件）
const char* dcr_tsunami_warning_name(uint8_t v);    // 津波警報コード 4bit
const char* dcr_tsunami_height_name(uint8_t v);     // 津波の高さ 4bit
const char* dcr_tsunami_region_name(uint16_t code); // 津波予報区 10bit（96件）
const char* dcr_long_period_name(uint8_t v);        // 長周期地震動階級
const char* dcr_eew_region_name(uint8_t code);      // EEW 対象地域（1〜80・80bitビットマップ用）
// 震央地名（10bit・EEW と震源が共通で持つ「どこの地震か」）。表は dcr_epicenter_table.cpp。
//   ★EEW は座標を持たないので、EEW における地名解決は**この関数が唯一の手段**になる。
const char* dcr_epicenter_name(uint16_t code);
// 防災上の留意事項（9bit・EEW/震源/津波/北西太平洋津波が 3 個ずつ持つ）。
//   表は dcr_prevention_table.cpp（一次資料 Table 4.1.2-6 から機械転記・全 55 件）。
//   ★この関数だけ**未知コードで nullptr を返す**（他は "?"）。仕様改版で新コードが
//     送られ得ると一次資料に明記があり、そのとき呼び出し側が「留意事項コード=307」と
//     数値を出せる余地を残すため。呼び出し側は null チェックが要る。
const char* dcr_prevention_name(uint16_t code);
const char* dcr_weather_state_name(uint8_t v);
const char* dcr_weather_sub_name(uint8_t v);
const char* dcr_weather_region_name(uint32_t code); // 府県予報区（75件）

// ── 2026-07-30 追加：残り7カテゴリの名称テーブル ─────────────────────────────
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

// 通報1件の人間可読な1行要約（時刻は JST へ変換済み）。
//   SD ログの補助・EPD 表示・シリアル出力で**同じ文面**を使うためにここに置く
//   （各所で書き分けると表記が割れ、ログの突き合わせが面倒になる）。
//   詳細デコーダのあるカテゴリは中身入り、無いカテゴリはカテゴリ名＋発令時刻。
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
//   ★地震発生時刻（DcrEew/DcrHypocenter の otDay/otHour/otMin）にも月が無いので、
//     同じ通報の報告時刻の月（r.month）を渡して変換すること。
//   既知の限界: 電文に年が無いため閏年を判定できない。2月28日 15:00 UTC 以降だけ
//   日付が1日ずれ得る（平年として扱う）。実用上は許容範囲と判断。
void dcr_to_jst(uint8_t* month, uint8_t* day, uint8_t* hour, uint8_t* minute);

// 内容キーを作る（dcr_decode が内部で呼び r.contentKey に入れる）。単体でも使える。
//   材料 = 共通部（MT/カテゴリ/報告時刻/情報種別）＋ 詳細デコーダが有るカテゴリは
//   その全フィールド。生バイトは一切混ぜない（それが不安定なのが今回の教訓）。
uint32_t dcr_content_key(const DcrReport& r);

// ── 低レベルユーティリティ（受信経路が PAB/MT の速判定に使う）───────────────
uint32_t dcr_bits(const uint8_t* msg, int bitOffset, int bitLen);  // MSB-first ビット読み
bool     dcr_pab_valid(uint8_t pab);   // プリアンブル 3 値の巡回（IS-QZSS-L1S §4.1.2.2）
