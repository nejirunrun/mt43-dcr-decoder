# mt43-dcr-decoder

QZSS L1S で放送される **災害・危機管理通報（DC Report, MT43/44）** の
250bit メッセージをデコードする、依存ゼロ（プラットフォーム非依存）の C++17
純関数ライブラリです。GNSS/QZSS受信機から復元した32byteの生メッセージを渡すと、
CRC24検証・共通部の抽出・カテゴリ別詳細（震源・津波・気象・台風など）の抽出まで
を行います。

## 特徴

- **状態を持たない純関数**。入力は32byte（`uint8_t msg[32]`）、出力は値。
  時刻や `millis()` を見ない・グローバル変数を持たない・スレッドセーフ。
- **Arduino / ESP32 非依存**。標準ライブラリ（`<cstdint>` 等）のみで動作し、
  Linux/macOS/Windows のデスクトップ環境やCIでもそのままビルド・単体テストできる。
- **CRC24検証・PAB（プリアンブル）検証込み**。生の32byteを渡すだけで妥当性確認から
  カテゴリ別詳細デコードまで行う。
- **全14カテゴリに対応**: 緊急地震速報・震源・震度・南海トラフ地震関連情報・津波・
  北西太平洋津波・火山・降灰・気象・洪水・台風・海上。
- **コード表を同梱**: 震央地名（345件）・火山名（121件）・沿岸地域（英語名・98件）・
  洪水予報区（392件）・市町村（1792件）・防災上の留意事項（55件）。

## 対象外（このライブラリがやらないこと）

- 受信（GNSS/QZSS受信機からのRXM-SFRBX等の生データ取得）はスコープ外です。
  すでにデコード済みの32byteメッセージを渡してください。
- 表示・通知・地域フィルタ・重複排除などの上位ロジックは含みません
  （`dcr_decode()` / 各 `dcr_xxx()` 関数と、`dcr_summary()` によるサマリ文字列生成まで）。

## 使い方

APIは「共通部を取り出す `dcr_decode()`」→「`category` に応じて詳細デコーダを呼ぶ」
の2段構えです。

```cpp
#include "dcr_report.h"

uint8_t msg[32] = { /* QZSS L1S 250bit メッセージ（32byte, MSB-first） */ };

DcrReport r;
if (dcr_decode(msg, r) && r.jmaValid) {
    if (r.category == DCR_CAT_HYPOCENTER) {
        DcrHypocenter h;
        if (dcr_hypocenter(r, h)) {
            // h.latitude, h.longitude, h.magRaw, h.depthRaw, h.epicenterCode ...
        }
    }
}
```

`DcrReport`（共通部: 通報区分・カテゴリ・報告時刻・種別）と、カテゴリごとの詳細構造体
（`DcrHypocenter` / `DcrTsunami` / `DcrWeather` 等、全14カテゴリ分）の対応は
[`src/dcr_report.h`](src/dcr_report.h) を参照してください。数値フィールドはコードの
まま入っているものが多く、表示用文字列が必要な場合は対応する `dcr_xxx_name()` で
変換します。

サンプルプログラム `examples/decode_sample.cpp` は、64桁の16進文字列
（32byteメッセージ）をコマンドライン引数で受け取り、デコード結果を表示します。

## ビルド

```bash
cmake -S . -B build
cmake --build build
./build/decode_sample <64桁の16進文字列>
```

ライブラリ単体を既存プロジェクトへ組み込む場合は、`src/` 以下の8ファイル
（`dcr_report.h/.cpp` と6つのコード表 `.cpp`）をそのままコピーするか、
CMakeの `add_subdirectory()` で `mt43_dcr_decoder` ターゲットをリンクしてください。
Arduino/PlatformIO環境でも `.ino`/`.cpp` と同様にビルドツリーへ追加するだけで動作します
（`<Arduino.h>` には依存していません）。

Windows + MinGW-w64（GCC）+ Ninja の組み合わせでビルド・実行を確認済みです
（`decode_sample.exe` の起動まで確認。実際の受信メッセージによる検証は別途必要です）。

## 一次資料・出典

- **一次資料**: IS-QZSS-DCR-016（内閣府/内閣府衛星測位システム推進局）。
  `qzss.go.jp` の同意ページ（`technical/download/is_qzss_dcr_016_agree.html`）経由で
  PDFを取得できます。ビット配置は一次資料の条項番号で本文中に明記しています。
- コード表（震央地名・津波予報区・火山名・市町村・沿岸地域・地方海上予報区・
  洪水予報区）はいずれも一次資料の該当Tableが出典です。
- 開発経緯・検証記録は [`docs/DECODER_NOTES.md`](docs/DECODER_NOTES.md) を参照してください。

## 利用条件についての注意

⚠ 災危通報の**内容をUIに表示する**用途で使う場合、内閣府/QZSSの
「Additional Terms of Use」により免責事項の表示等、追加の利用条件が課されています。
デコードのみ（内部処理）であれば発生しませんが、エンドユーザー向けに文言を
表示するアプリケーションを作る場合は、一次資料の同意ページで利用条件を確認のうえ
対応してください。

このライブラリが同梱するコード表（震央地名・火山名・市町村名・府県予報区など）は
IS-QZSS-DCR-016 のParameter Definitionsを機械的に転記したものです。転記・再配布に
関する条件も同じ同意ページに定められているため、あわせて確認してください。

## ライセンス

このリポジトリのソースコード（C++実装）は MIT License です。`LICENSE` 参照。
このライセンスは上記の内閣府/QZSSの利用条件とは別物で、これに代わるものではありません
（本ソフトウェアの著作権許諾のみを定めています）。

## 設計判断の詳細

重複判定の作り方・EEWと震源の取り違え防止・発令時刻がUTCである点・通報区分(Rc)の
コード表など、実装にあたって踏んだ落とし穴や実測知見は
[`docs/DECODER_NOTES.md`](docs/DECODER_NOTES.md) にまとめています。
仕様の解釈で迷ったときや、新しいフィールドを追加するときに参照してください。
