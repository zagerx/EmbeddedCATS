/* 全局调色板与 QSS 加载接口。 */

#ifndef THEME_H
#define THEME_H

class QApplication;

namespace theme
{
namespace color
{
inline constexpr const char *kBg = "#171a1f";
inline constexpr const char *kDeep = "#11141a";
inline constexpr const char *kSurface = "#1f242b";
inline constexpr const char *kBorder = "#333a44";
inline constexpr const char *kDivider = "#3d4552"; /* 区域内部细分隔线，比边框稍亮 */
inline constexpr const char *kFg = "#dfe4ea";
inline constexpr const char *kFgDim = "#8b949e";
inline constexpr const char *kPrimary = "#3d8bfd";
inline constexpr const char *kOk = "#3fb950";
inline constexpr const char *kWarn = "#d29922";
inline constexpr const char *kFault = "#f85149";
} // namespace color

/* 读 :/theme/app.qss，替换占位符后设为全局样式表。 */
void apply(QApplication &app);
} // namespace theme

#endif /* THEME_H */
