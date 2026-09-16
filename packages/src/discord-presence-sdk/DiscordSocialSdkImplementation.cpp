// Discord Social SDKの実装をこの翻訳単位へ展開します。
//
// discordpp.h はヘッダーオンリーのラッパーで、DISCORDPP_IMPLEMENTATION
// を定義した**ちょうど1つ**の.cppが実装を持つ決まりです。0個だと
// 「未解決の外部シンボル」、2個以上だと「多重定義」になります。
// このパッケージではこのファイルがその1つです。
//
// 自分のコードからSDKを使いたい場合も、DISCORDPP_IMPLEMENTATIONは
// **定義しないで**ください。discordpp.h をそのままincludeすれば
// 使えます。
#if defined(LAMAPON_DISCORD_SOCIAL_SDK)

#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>

#endif
