// dcr_coastal_region_table.cpp — 北西太平洋津波 沿岸地域コード（7bit）→ 地名 の対応表
// =============================================================================
// 【役割】北西太平洋津波(disaster_category=6)専用の地域表。国内向け（震央地名・
//   都道府県等）と違い、この表だけは**英語名のみ**（azarashi にも日本語版が
//   存在しない＝カムチャツカ・沿海州・台湾等の海外地名で、国内での日本語表記が
//   一次資料側にそもそも無いため）。README「実装状況」に記載の通り、意図的に
//   実装を見送っていたカテゴリだが、2026-07-30 に全カテゴリ実装の一環で追加した。
//
// 【"Reserved" について】
//   仕様上「予約（未使用）」と定義されているコードがある。未定義コード（"?"）とは
//   区別する＝この値は仕様書に明記された「使われていない」であって「知らない」
//   ではない。azarashi のテーブルをそのまま転記している。
//
// 【出典】azarashi（MIT・nbtk 氏）qzss_dcr_jma_coastal_region.py から機械抽出（2026-07-30）。
// =============================================================================
#include "dcr_report.h"

struct CoastalRegionEntry { uint8_t code; const char* name; };

static const CoastalRegionEntry kCoastalRegions[] = {
{1,"Ust-Kamchatsk (East Coasts of Kamchatka Peninsula)"},{2,"Petropavlovsk-K (East Coasts of Kamchatka Peninsula)"},{3,"Severo Kurilsk (Kuril Islands)"},{4,"Urup Islands (Kuril Islands)"},
{5,"Busan (South Coasts of Korean Peninsula)"},{6,"Nohwa (South Coasts of Korean Peninsula)"},{7,"Seogwipo (South Coasts of Korean Peninsula)"},{8,"Hualien (Taiwan)"},
{9,"Basco (East Coasts of Philippines)"},{10,"Palanan (East Coasts of Philippines)"},{11,"Legaspi (East Coasts of Philippines)"},{12,"Laoang (East Coasts of Philippines)"},
{13,"Madrid (East Coasts of Philippines)"},{14,"Davao (East Coasts of Philippines)"},{15,"Berebere (North Coasts of Irian Jaya)"},{16,"Patani (North Coasts of Irian Jaya)"},
{17,"Sorong (North Coasts of Irian Jaya)"},{18,"Manokwari (North Coasts of Irian Jaya)"},{19,"Warsa (North Coasts of Irian Jaya)"},{20,"Jayapura (North Coasts of Irian Jaya)"},
{21,"Vanimo (North Coasts of Papua New Guinea)"},{22,"Wewak (North Coasts of Papua New Guinea)"},{23,"Madang (North Coasts of Papua New Guinea)"},{24,"Manus Islands (North Coasts of Papua New Guinea)"},
{25,"Rabaul (North Coasts of Papua New Guinea)"},{26,"Kavieng (North Coasts of Papua New Guinea)"},{27,"Kimbe (North Coasts of Papua New Guinea)"},{28,"Kieta (North Coasts of Papua New Guinea)"},
{29,"Guam (Mariana Islands)"},{30,"Saipan (Mariana Islands)"},{31,"Malakal (Palau)"},{32,"Yap Island (Micronesia)"},
{33,"Chuuk Island (Micronesia)"},{34,"Pohnpei Island (Micronesia)"},{35,"Kosrae Island (Micronesia)"},{36,"Eniwetok Island (Marshall Islands)"},
{37,"Panggoe (North Coasts of Solomon Islands)"},{38,"Auki (North Coasts of Solomon Islands)"},{39,"Kirakira (North Coasts of Solomon Islands)"},{40,"Munda (Solomon Sea)"},
{41,"Honiara (Solomon Sea)"},{42,"Reserved"},{43,"Reserved"},{44,"Reserved"},
{45,"Reserved"},{46,"Reserved"},{47,"Reserved"},{48,"Reserved"},
{49,"Reserved"},{50,"Reserved"},{51,"Reserved"},{52,"Reserved"},
{53,"Reserved"},{54,"Reserved"},{55,"Reserved"},{56,"Reserved"},
{57,"Reserved"},{58,"Reserved"},{59,"Reserved"},{60,"Reserved"},
{61,"Reserved"},{62,"Reserved"},{63,"Reserved"},{64,"Reserved"},
{65,"Reserved"},{66,"Ostrov-Karaginskiy (East Coasts of Kamchatka Peninsula)"},{67,"Nikolskoya (East Coasts of Kamchatka Peninsula)"},{68,"Tongyeong (South Coasts of Korean Peninsula)"},
{69,"Heuksando (South Coasts of Korean Peninsula)"},{70,"Cheju-Island (South Coasts of Korean Peninsula)"},{71,"Chilung (Taiwan)"},{72,"Taitung (Taiwan)"},
{73,"Reserved"},{74,"Homel (Taiwan)"},{75,"Geme (North Coasts of Irian Jaya)"},{76,"Ulamona (North Coasts of Papua New Guinea)"},
{77,"Ghatere (North Coasts of Solomon Islands)"},{78,"Amun (Solomon Sea)"},{79,"Falamae (Solomon Sea)"},{80,"Misima (Solomon Sea)"},
{81,"Alotau (Solomon Sea)"},{82,"Lae (Solomon Sea)"},{83,"Port-Moresby (Coral Sea)"},{84,"Shanghai (Coasts of East China Sea)"},
{85,"Zhoushan (Coasts of East China Sea)"},{86,"Wenzhou (Coasts of East China Sea)"},{87,"Reserved"},{88,"Reserved"},
{89,"Reserved"},{90,"Reserved"},{91,"Reserved"},{92,"Reserved"},
{93,"Reserved"},{94,"Reserved"},{95,"Reserved"},{96,"Reserved"},
{99,"Unknown"},{100,"Other region"},
};
static const int kCoastalRegionCount = sizeof(kCoastalRegions) / sizeof(kCoastalRegions[0]);

const char* dcr_coastal_region_name(uint8_t code) {
    for (int i = 0; i < kCoastalRegionCount; i++)
        if (kCoastalRegions[i].code == code) return kCoastalRegions[i].name;
    return "?";
}
