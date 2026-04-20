#include "core/bot.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <random>
#include <algorithm>
#include <sstream>
#include <Windows.h>

using Clock = std::chrono::steady_clock;

// ── UTF-8 字符级编辑距离（允许单字缺失/替换/插入） ──

/// 将 UTF-8 字符串拆为 Unicode code-point 序列
static std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = 0;
        unsigned char c = s[i];
        int len = 1;
        if (c < 0x80)      { cp = c; }
        else if (c < 0xC0) { cp = c; }
        else if (c < 0xE0) { cp = c & 0x1F; len = 2; }
        else if (c < 0xF0) { cp = c & 0x0F; len = 3; }
        else               { cp = c & 0x07; len = 4; }
        for (int j = 1; j < len && i + j < s.size(); j++)
            cp = (cp << 6) | (s[i + j] & 0x3F);
        cps.push_back(cp);
        i += len;
    }
    return cps;
}

/// 字符级 Levenshtein 编辑距离，early-exit 当距离 > max_dist
static int edit_distance(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int max_dist = 2) {
    int m = (int)a.size(), n = (int)b.size();
    if (std::abs(m - n) > max_dist) return max_dist + 1;
    // 单行 DP
    std::vector<int> dp(n + 1);
    for (int j = 0; j <= n; j++) dp[j] = j;
    for (int i = 1; i <= m; i++) {
        int prev = dp[0];
        dp[0] = i;
        int row_min = dp[0];
        for (int j = 1; j <= n; j++) {
            int tmp = dp[j];
            if (a[i - 1] == b[j - 1]) {
                dp[j] = prev;
            } else {
                dp[j] = 1 + std::min({prev, dp[j], dp[j - 1]});
            }
            prev = tmp;
            row_min = std::min(row_min, dp[j]);
        }
        if (row_min > max_dist) return max_dist + 1;
    }
    return dp[n];
}

/// 在候选集合中模糊查找：编辑距离 ≤ 1 且无歧义
static std::optional<std::string> fuzzy_match_set(const std::string& text,
                                                   const std::set<std::string>& candidates) {
    auto text_cp = utf8_to_codepoints(text);
    std::string best;
    int best_dist = 2; // threshold + 1
    int match_count = 0;
    for (auto& cand : candidates) {
        auto cand_cp = utf8_to_codepoints(cand);
        int d = edit_distance(text_cp, cand_cp, 1);
        if (d < best_dist) {
            best_dist = d;
            best = cand;
            match_count = 1;
        } else if (d == best_dist) {
            match_count++;
        }
    }
    // 编辑距离 ≤ 1 且无歧义
    if (best_dist <= 1 && match_count == 1)
        return best;
    return std::nullopt;
}

static void sleep_sec(double sec) {
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(sec * 1000)));
}

static std::string basename_no_ext(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    std::string name = (pos != std::string::npos) ? path.substr(pos + 1) : path;
    auto dot = name.rfind('.');
    return (dot != std::string::npos) ? name.substr(0, dot) : name;
}

// ── 热键 VK 映射 ──
static int hotkey_name_to_vk(const std::string& name) {
    if (name == "delete")   return VK_DELETE;
    if (name == "insert")   return VK_INSERT;
    if (name == "home")     return VK_HOME;
    if (name == "end")      return VK_END;
    if (name == "page up")  return VK_PRIOR;
    if (name == "page down")return VK_NEXT;
    if (name == "pause")    return VK_PAUSE;
    if (name == "esc")      return VK_ESCAPE;
    if (name == "f1")  return VK_F1;   if (name == "f2")  return VK_F2;
    if (name == "f3")  return VK_F3;   if (name == "f4")  return VK_F4;
    if (name == "f5")  return VK_F5;   if (name == "f6")  return VK_F6;
    if (name == "f7")  return VK_F7;   if (name == "f8")  return VK_F8;
    if (name == "f9")  return VK_F9;   if (name == "f10") return VK_F10;
    if (name == "f11") return VK_F11;  if (name == "f12") return VK_F12;
    if (name == "space")    return VK_SPACE;
    if (name.size() == 1 && std::isalpha((unsigned char)name[0]))
        return VkKeyScanA(name[0]) & 0xFF;
    return VK_DELETE; // 默认Delete键
}

// ── CurrencyWarBot ──

CurrencyWarBot::CurrencyWarBot(WindowController& window, OCREngine& ocr,
                                 ImageMatcher& matcher, const AppConfig& config)
    : window_(window), ocr_(ocr), matcher_(matcher), config_(config)
{
    auto strat_list = load_name_list("res/strategy.txt");
    auto deb_list   = load_name_list("res/debuff.txt");
    strategies_.insert(strat_list.begin(), strat_list.end());
    debuffs_.insert(deb_list.begin(), deb_list.end());
    std::cout << "[bot] 已加载 " << strategies_.size()
              << " 个合法投资策略, " << debuffs_.size() << " 个已知Debuff" << std::endl;
}

void CurrencyWarBot::stop() {
    stop_event_ = true;
    std::cout << "[bot] 收到停止信号，将在当前步骤完成后退出。" << std::endl;
}

bool CurrencyWarBot::stopped() const {
    return stop_event_.load();
}

// ── 截图 ──

cv::Mat CurrencyWarBot::shot() {
    // PrintWindow 直接从窗口取内容，不受覆盖层遮挡影响
    return window_.screenshot(true);
}

// ── 覆盖层辅助 ──

void CurrencyWarBot::init_overlay() {
    try {
        auto [x, y, r, b] = window_.get_client_rect();
        overlay_ = std::make_unique<ScreenOverlay>();
        overlay_->start(x, y, r - x, b - y);
    } catch (const std::exception& e) {
        std::cerr << "[bot] 覆盖层初始化失败: " << e.what() << std::endl;
        overlay_.reset();
    }
}

void CurrencyWarBot::reposition_overlay() {
    if (overlay_) {
        try {
            auto [x, y, r, b] = window_.get_client_rect();
            overlay_->reposition(x, y, r - x, b - y);
        } catch (...) {}
    }
}

void CurrencyWarBot::olog(const std::string& msg) {
    if (overlay_) overlay_->log(msg);
}

void CurrencyWarBot::ostep(const std::string& step) {
    if (overlay_) overlay_->set_step(step);
    std::cout << "[bot] 当前步骤: " << step << std::endl;
}

void CurrencyWarBot::mark_ocr(const std::vector<OCRResult>& results,
                                const std::set<std::string>& keywords) {
    if (!overlay_ || !config_.show_debug_overlay) return;
    std::vector<Mark> marks;
    for (auto& r : results) {
        int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
        for (auto& [px, py] : r.box) {
            x1 = std::min(x1, px); y1 = std::min(y1, py);
            x2 = std::max(x2, px); y2 = std::max(y2, py);
        }
        bool is_target = false;
        if (!keywords.empty()) {
            for (auto& kw : keywords) {
                if (r.text.find(kw) != std::string::npos) {
                    is_target = true;
                    break;
                }
            }
        }
        COLORREF color = is_target ? COLOR_TARGET : COLOR_OCR;
        marks.emplace_back(x1, y1, x2, y2, color, r.text);
    }
    overlay_->update_marks(marks);
}

void CurrencyWarBot::mark_match(const std::optional<MatchResult>& match,
                                  const std::string& label) {
    if (!overlay_ || !config_.show_debug_overlay || !match) return;
    auto [l, t, r, b] = match->rect();
    overlay_->update_marks({Mark(l, t, r, b, COLOR_MATCH, label)});
}

void CurrencyWarBot::clear_marks() {
    if (overlay_) overlay_->clear();
}

// ── 底层工具 ──

bool CurrencyWarBot::wait_and_click_text(const std::string& text,
                                          double timeout, double post_delay, int retries) {
    for (int attempt = 1; attempt <= retries; ++attempt) {
        auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(timeout * 1000));
        while (Clock::now() < deadline && !stopped()) {
            reposition_overlay();
            cv::Mat img = shot();
            auto all_results = ocr_.recognize(img);
            mark_ocr(all_results, {text});

            for (auto& item : all_results) {
                if (item.confidence >= 0.5f && item.text.find(text) != std::string::npos) {
                    auto [cx, cy] = item.center();
                    olog("识别文字 [" + text + "] → (" + std::to_string(cx) + "," + std::to_string(cy) + ")");
                    std::cout << "[bot] [尝试" << attempt << "/" << retries
                              << "] 识别到文字 '" << text << "' 坐标=(" << cx << "," << cy << ")" << std::endl;
                    sleep_sec(CLICK_DELAY);
                    window_.click(cx, cy);
                    clear_marks();
                    sleep_sec(post_delay);
                    return true;
                }
            }
            sleep_sec(POLL);
        }
        if (stopped()) break;
        if (attempt < retries) {
            olog("未找到文字 [" + text + "]，重试 " + std::to_string(attempt) + "/" + std::to_string(retries));
        } else {
            olog("超时：文字 [" + text + "] 未找到");
        }
    }
    clear_marks();
    return false;
}

bool CurrencyWarBot::wait_and_click_image(const std::string& path,
                                            double timeout, double post_delay, int retries) {
    std::string label = basename_no_ext(path);
    for (int attempt = 1; attempt <= retries; ++attempt) {
        auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(timeout * 1000));
        while (Clock::now() < deadline && !stopped()) {
            reposition_overlay();
            cv::Mat img = shot();
            auto match = matcher_.find(img, path);
            if (match) {
                mark_match(match, label);
                auto [cx, cy] = match->center();
                olog("识别图像 [" + label + "] → (" + std::to_string(cx) + "," + std::to_string(cy) + ")");
                std::cout << "[bot] [尝试" << attempt << "/" << retries
                          << "] 识别到图像 '" << path << "' 坐标=(" << cx << "," << cy
                          << ") 置信度=" << match->confidence << std::endl;
                sleep_sec(CLICK_DELAY);
                window_.click(cx, cy);
                clear_marks();
                sleep_sec(post_delay);
                return true;
            }
            sleep_sec(POLL);
        }
        if (stopped()) break;
        if (attempt < retries) {
            olog("未找到图像 [" + label + "]，重试 " + std::to_string(attempt) + "/" + std::to_string(retries));
        } else {
            olog("超时：图像 [" + label + "] 未找到");
        }
    }
    clear_marks();
    return false;
}

bool CurrencyWarBot::wait_for_image(const std::string& path, double timeout) {
    std::string label = basename_no_ext(path);
    auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(timeout * 1000));
    while (Clock::now() < deadline && !stopped()) {
        reposition_overlay();
        cv::Mat img = shot();
        auto match = matcher_.find(img, path);
        if (match) {
            mark_match(match, label);
            olog("检测到图像 [" + label + "]");
            std::cout << "[bot] 检测到图像 '" << path << "'" << std::endl;
            return true;
        }
        sleep_sec(POLL);
    }
    if (!stopped()) {
        olog("超时：图像 [" + label + "] 未出现");
    }
    return false;
}

// ── OCR 扫描 ──

std::vector<OCRResult> CurrencyWarBot::scan_env_region() {
    reposition_overlay();
    cv::Mat img = shot();
    int w = img.cols;
    auto& er = config_.env_region;
    int region_w = (er.w > 0) ? er.w : w;
    auto results = ocr_.recognize_region(img, er.x, er.y, region_w, er.h);
    std::set<std::string> kw(config_.target_envs.begin(), config_.target_envs.end());
    mark_ocr(results, kw);
    return results;
}

std::vector<OCRResult> CurrencyWarBot::scan_debuff_region() {
    reposition_overlay();
    cv::Mat img = shot();
    auto& dr = config_.debuff_region;
    auto results = ocr_.recognize_region(img, dr.x, dr.y, dr.w, dr.h);
    std::set<std::string> kw;
    for (auto& s : config_.unwanted_debuffs) kw.insert(s);
    for (auto& s : config_.wanted_buffs) kw.insert(s);
    mark_ocr(results, kw);
    return results;
}

std::optional<std::string> CurrencyWarBot::match_strategy(const std::string& text) const {
    if (strategies_.empty()) return text;
    std::string clean = text;
    // trim
    while (!clean.empty() && std::isspace((unsigned char)clean.front())) clean.erase(clean.begin());
    while (!clean.empty() && std::isspace((unsigned char)clean.back())) clean.pop_back();

    // 1. 精确匹配
    if (strategies_.count(clean)) return clean;
    // 2. 子串匹配
    for (auto& s : strategies_) {
        if (s.find(clean) != std::string::npos || clean.find(s) != std::string::npos) {
            return s;
        }
    }
    // 3. 歧义消解特例：OCR 丢字导致模糊匹配歧义的已知情况
    //    "战力升" → "战力飙升"
    if (clean == "战力升" && strategies_.count("战力飙升")) {
        return "战力飙升";
    }
    // 4. 模糊匹配：编辑距离 ≤ 1 且无歧义
    return fuzzy_match_set(clean, strategies_);
}

std::optional<std::string> CurrencyWarBot::match_debuff(const std::string& text) const {
    if (debuffs_.empty()) return text;
    std::string clean = text;
    while (!clean.empty() && std::isspace((unsigned char)clean.front())) clean.erase(clean.begin());
    while (!clean.empty() && std::isspace((unsigned char)clean.back())) clean.pop_back();

    // 1. 精确匹配
    if (debuffs_.count(clean)) return clean;
    // 2. 子串匹配
    for (auto& d : debuffs_) {
        if (d.find(clean) != std::string::npos || clean.find(d) != std::string::npos) {
            return d;
        }
    }
    // 3. 模糊匹配：编辑距离 ≤ 1 且无歧义
    return fuzzy_match_set(clean, debuffs_);
}

std::vector<CurrencyWarBot::ValidatedResult>
CurrencyWarBot::validate_env_results(const std::vector<OCRResult>& results) const {
    std::vector<ValidatedResult> validated;
    for (auto& r : results) {
        auto matched = match_strategy(r.text);
        if (matched) {
            ValidatedResult vr;
            vr.ocr = r;
            vr.matched_strategy = *matched;
            vr.center = r.center();
            validated.push_back(std::move(vr));
        }
    }
    return validated;
}

std::vector<std::string>
CurrencyWarBot::validate_debuff_results(const std::vector<OCRResult>& results) const {
    std::vector<std::string> matched;
    for (auto& r : results) {
        auto name = match_debuff(r.text);
        if (name) matched.push_back(*name);
    }
    return matched;
}

std::vector<std::string> CurrencyWarBot::stable_scan_debuffs() {
    int min_rounds = config_.min_confirm_rounds;
    int max_attempts = config_.max_confirm_attempts;
    ostep("阶段一 [4/6] 稳定扫描Debuff…");
    std::cout << "[bot] 开始稳定扫描Debuff: 至少 " << min_rounds << " 次连续比对…" << std::endl;

    int consecutive = 0;
    std::vector<std::string> last_names;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (stopped()) return {};

        auto results = scan_debuff_region();
        auto debuff_names = validate_debuff_results(results);

        std::vector<std::string> sorted_names = debuff_names;
        std::sort(sorted_names.begin(), sorted_names.end());
        int count = (int)debuff_names.size();
        bool valid_count = (count >= DEBUFF_MIN && count <= DEBUFF_MAX);

        if (valid_count && sorted_names == last_names) {
            ++consecutive;
            std::ostringstream oss;
            oss << "Debuff比对#" << attempt << " [";
            for (size_t i = 0; i < sorted_names.size(); ++i) {
                if (i) oss << ", ";
                oss << sorted_names[i];
            }
            oss << "] ✓ (" << consecutive << "/" << min_rounds << ")";
            std::cout << "[bot]   " << oss.str() << std::endl;
            olog(oss.str());
        } else {
            consecutive = valid_count ? 1 : 0;
            std::ostringstream oss;
            oss << "Debuff比对#" << attempt << " ";
            if (valid_count) {
                oss << "[";
                for (size_t i = 0; i < sorted_names.size(); ++i) {
                    if (i) oss << ", ";
                    oss << sorted_names[i];
                }
                oss << "] (新组合 1/" << min_rounds << ")";
            } else {
                oss << "识别" << count << "个(需" << DEBUFF_MIN << "~" << DEBUFF_MAX << ")";
            }
            std::cout << "[bot]   " << oss.str() << std::endl;
            olog(oss.str());
        }

        last_names = sorted_names;

        if (consecutive >= min_rounds) {
            std::ostringstream oss;
            oss << "Debuff扫描完成: ";
            for (size_t i = 0; i < sorted_names.size(); ++i) {
                if (i) oss << ", ";
                oss << sorted_names[i];
            }
            std::cout << "[bot] " << oss.str() << std::endl;
            olog(oss.str());

            // 标注想要/不想要
            std::set<std::string> wanted_set(config_.wanted_buffs.begin(), config_.wanted_buffs.end());
            std::set<std::string> unwanted_set(config_.unwanted_debuffs.begin(), config_.unwanted_debuffs.end());

            for (auto& d : sorted_names) {
                if (wanted_set.count(d)) olog("✓ 想要的Debuff: " + d);
                if (unwanted_set.count(d)) olog("⚠ 不想要的Debuff: " + d);
            }

            return std::vector<std::string>(sorted_names.begin(), sorted_names.end());
        }

        sleep_sec(POLL);
    }

    olog("Debuff稳定扫描失败");
    return {};
}

std::vector<CurrencyWarBot::ValidatedResult>
CurrencyWarBot::stable_scan_env(int expected_count) {
    int min_rounds = config_.min_confirm_rounds;
    int max_attempts = config_.max_confirm_attempts;
    ostep("阶段二 稳定扫描投资策略…");
    std::cout << "[bot] 开始稳定扫描: 至少 " << min_rounds
              << " 次连续比对(期望" << expected_count << "个)…" << std::endl;

    int consecutive = 0;
    std::vector<std::string> last_names;
    std::vector<ValidatedResult> last_validated;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (stopped()) return {};

        auto results = scan_env_region();
        auto validated = validate_env_results(results);

        std::vector<std::string> current_names;
        for (auto& v : validated) current_names.push_back(v.matched_strategy);
        std::sort(current_names.begin(), current_names.end());

        if ((int)validated.size() == expected_count && current_names == last_names) {
            ++consecutive;
            std::ostringstream oss;
            oss << "比对#" << attempt << " [";
            for (size_t i = 0; i < current_names.size(); ++i) {
                if (i) oss << ", ";
                oss << current_names[i];
            }
            oss << "] ✓ (" << consecutive << "/" << min_rounds << ")";
            std::cout << "[bot]   " << oss.str() << std::endl;
            olog(oss.str());
        } else {
            consecutive = ((int)validated.size() == expected_count) ? 1 : 0;
            std::ostringstream oss;
            oss << "比对#" << attempt << " 识别" << validated.size()
                << "个(需" << expected_count << ")";
            std::cout << "[bot]   " << oss.str() << std::endl;
            olog(oss.str());
        }

        last_names = current_names;
        last_validated = validated;

        if (consecutive >= min_rounds) {
            std::ostringstream oss;
            oss << "扫描完成: ";
            for (size_t i = 0; i < current_names.size(); ++i) {
                if (i) oss << ", ";
                oss << current_names[i];
            }
            std::cout << "[bot] " << oss.str() << std::endl;
            olog(oss.str());
            return last_validated;
        }

        sleep_sec(POLL);
    }

    olog("稳定扫描失败");
    return {};
}

std::optional<CurrencyWarBot::ValidatedResult>
CurrencyWarBot::find_target_env(const std::vector<ValidatedResult>& results) const {
    std::set<std::string> targets(config_.target_envs.begin(), config_.target_envs.end());
    for (auto& r : results) {
        for (auto& kw : targets) {
            if (r.matched_strategy.find(kw) != std::string::npos) {
                return r;
            }
        }
    }
    return std::nullopt;
}

std::vector<OCRResult> CurrencyWarBot::wait_for_env_screen(double timeout) {
    auto deadline = Clock::now() + std::chrono::milliseconds(static_cast<int>(timeout * 1000));
    while (Clock::now() < deadline && !stopped()) {
        auto results = scan_env_region();
        if (!results.empty()) return results;
        sleep_sec(POLL);
    }
    std::cout << "[bot] 超时: 投资环境界面未出现。" << std::endl;
    return {};
}

// ── 阶段一 ──

bool CurrencyWarBot::phase1() {
    struct Step { std::string path; std::string desc; };
    std::vector<Step> pre_steps = {
        {"res/buttons/开始货币战争.png", "阶段一 [1/6] 开始货币战争"},
        {"res/buttons/进入标准博弈.png", "阶段一 [2/6] 进入标准博弈"},
        {"res/buttons/开始对局.png",     "阶段一 [3/6] 开始对局"},
    };
    std::cout << "[bot] ----- 阶段一：进入货币战争流程 -----" << std::endl;
    olog("----- 阶段一：进入货币战争 -----");

    for (auto& [path, desc] : pre_steps) {
        if (stopped()) return false;
        ostep(desc);
        if (!wait_and_click_image(path)) return false;
    }

    // Step 4/6: 等待下一步 & 扫描Debuff
    if (!stopped()) {
        ostep("阶段一 [4/6] 等待下一步 & 扫描Debuff");
        if (!wait_for_image("res/buttons/下一步_开局.png")) {
            olog("等待下一步超时");
            return false;
        }
        last_debuffs_ = stable_scan_debuffs();
        if (last_debuffs_.empty() && !stopped()) {
            olog("Debuff扫描失败，继续流程");
        }
    }

    // Step 5/6: 下一步
    if (!stopped()) {
        ostep("阶段一 [5/6] 下一步");
        if (!wait_and_click_image("res/buttons/下一步_开局.png")) return false;
    }

    // Step 6/6: 点击空白处继续
    if (!stopped()) {
        ostep("阶段一 [6/6] 点击空白处继续");
        if (!wait_and_click_image("res/buttons/点击空白处继续.png")) return false;
    }
    return true;
}

// ── 阶段二 ──

std::string CurrencyWarBot::phase2() {
    std::cout << "[bot] ----- 阶段二：选择投资环境 -----" << std::endl;
    olog("----- 阶段二：选择投资环境 -----");
    ostep("阶段二 等待投资环境界面…");

    auto results = wait_for_env_screen();
    if (results.empty() || stopped()) return "";

    // 第一次稳定扫描
    auto validated = stable_scan_env();
    if (validated.empty() || stopped()) {
        olog("稳定扫描未成功");
        return "";
    }

    auto target = find_target_env(validated);
    if (target) {
        ostep("阶段二 找到目标: " + target->matched_strategy);
        olog("★ 目标命中: " + target->matched_strategy);
        sleep_sec(CLICK_DELAY);
        window_.click(target->center.first, target->center.second);
        sleep_sec(STEP_DELAY);
        wait_and_click_image("res/buttons/确认.png", TIMEOUT_SHORT);
        return target->matched_strategy;
    }

    ostep("阶段二 刷新投资环境…");
    olog("未找到目标，尝试刷新");

    // 刷新
    bool refreshed = wait_and_click_image("res/buttons/refresh.png", 5.0, 1.2);
    if (refreshed && !stopped()) {
        validated = stable_scan_env();
        if (!validated.empty()) {
            target = find_target_env(validated);
            if (target) {
                ostep("阶段二 刷新后找到: " + target->matched_strategy);
                olog("★ 刷新后命中: " + target->matched_strategy);
                sleep_sec(CLICK_DELAY);
                window_.click(target->center.first, target->center.second);
                sleep_sec(STEP_DELAY);
                wait_and_click_image("res/buttons/确认.png", TIMEOUT_SHORT);
                return target->matched_strategy;
            }
        }
        olog("刷新后仍未找到目标");
    } else {
        olog("未找到刷新按钮，随机选择");
    }

    // 蓝海优先 / 随机选择
    if (validated.empty() && refreshed) {
        validated = stable_scan_env();
    }

    if (!validated.empty()) {
        // 刷新后优先选择蓝海
        if (refreshed) {
            for (auto& r : validated) {
                if (r.matched_strategy.find("蓝海") != std::string::npos) {
                    ostep("阶段二 优先选择蓝海: " + r.matched_strategy);
                    olog("优先选择蓝海: " + r.matched_strategy);
                    sleep_sec(CLICK_DELAY);
                    window_.click(r.center.first, r.center.second);
                    sleep_sec(STEP_DELAY);
                    wait_and_click_image("res/buttons/确认.png", TIMEOUT_SHORT);

                    // 蓝海额外处理
                    olog("蓝海额外确认：扫描投资环境…");
                    auto lanhai_validated = stable_scan_env(1);
                    std::string lanhai_inner_strategy = r.matched_strategy;
                    if (!lanhai_validated.empty()) {
                        auto& chosen = lanhai_validated[0];
                        olog("蓝海：点击投资策略 " + chosen.matched_strategy);
                        sleep_sec(CLICK_DELAY);
                        window_.click(chosen.center.first, chosen.center.second);
                        sleep_sec(STEP_DELAY);
                        // 蓝海随机到目标策略时，视为命中目标
                        auto lanhai_target = find_target_env(lanhai_validated);
                        if (lanhai_target) {
                            lanhai_inner_strategy = lanhai_target->matched_strategy;
                            olog("★ 蓝海命中目标策略: " + lanhai_inner_strategy);
                        }
                    }
                    wait_and_click_image("res/buttons/确认.png", TIMEOUT_SHORT);
                    return lanhai_inner_strategy;
                }
            }
        }

        // 随机选择
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, (int)validated.size() - 1);
        auto& chosen = validated[dist(rng)];
        ostep("阶段二 随机选择: " + chosen.matched_strategy);
        olog("随机选择: " + chosen.matched_strategy);
        sleep_sec(CLICK_DELAY);
        window_.click(chosen.center.first, chosen.center.second);
        sleep_sec(STEP_DELAY);
        wait_and_click_image("res/buttons/确认.png", TIMEOUT_SHORT);
        return chosen.matched_strategy;
    }

    std::cout << "[bot] 投资环境区域未识别到合法策略，无法选择。" << std::endl;
    return "";
}

// ── 阶段三 ──

void CurrencyWarBot::phase3_exit() {
    std::cout << "[bot] ----- 阶段三：退出并重置 -----" << std::endl;
    olog("----- 阶段三：退出重置 -----");

    ostep("阶段三 等待界面加载…");
    olog("等待 " + std::to_string((int)SCENE_LOAD_DELAY) + "s 界面加载…");
    sleep_sec(SCENE_LOAD_DELAY);

    struct Step { std::string path; std::string desc; };
    std::vector<Step> exit_steps = {
        {"res/buttons/exit.png",       "阶段三 [1/5] 退出"},
        {"res/buttons/放弃并结算.png",  "阶段三 [2/5] 放弃并结算"},
        {"res/buttons/下一步_结算.png",   "阶段三 [3/5] 下一步"},
        {"res/buttons/下一页.png",     "阶段三 [4/5] 下一页"},
        {"res/buttons/返回货币战争.png","阶段三 [5/5] 返回货币战争"},
    };
    for (auto& [path, desc] : exit_steps) {
        if (stopped()) return;
        ostep(desc);
        wait_and_click_image(path);
    }
    sleep_sec(1.0);
}

// ── 主循环 ──

void CurrencyWarBot::run() {
    stop_event_ = false;

    // 注册全局热键
    int vk = hotkey_name_to_vk(config_.stop_hotkey);
    UINT mod = 0; // 无修饰键
    // 使用 RegisterHotKey 需要在有消息循环的线程中，改用轮询检测
    std::cout << "[bot] ★ 自动化启动 —— 按 " << config_.stop_hotkey << " 键随时停止 ★" << std::endl;

    init_overlay();

    // 热键轮询线程
    std::atomic<bool> hotkey_running{true};
    std::thread hotkey_thread([&]() {
        while (hotkey_running && !stopped()) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                stop();
                if (on_stopped) on_stopped();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    try {
        int iteration = 0;
        while (!stopped()) {
            ++iteration;
            std::cout << "[bot] ========== 第 " << iteration << " 轮循环开始 ==========" << std::endl;
            olog("===== 第 " + std::to_string(iteration) + " 轮循环 =====");
            ostep("第 " + std::to_string(iteration) + " 轮 开始");

            // 阶段一
            if (!phase1()) {
                if (!stopped()) {
                    olog("阶段一失败，终止");
                    std::cerr << "[bot] 阶段一失败，终止。" << std::endl;
                }
                break;
            }

            if (stopped()) break;

            // Debuff 检查
            bool debuff_bad = false;
            if (!last_debuffs_.empty()) {
                std::set<std::string> unwanted(config_.unwanted_debuffs.begin(),
                                                config_.unwanted_debuffs.end());
                std::set<std::string> wanted(config_.wanted_buffs.begin(),
                                              config_.wanted_buffs.end());

                for (auto& d : last_debuffs_) {
                    if (unwanted.count(d)) {
                        olog("⚠ 不想要的Debuff出现: " + d);
                        debuff_bad = true;
                    }
                }
                if (!wanted.empty()) {
                    for (auto& w : wanted) {
                        bool found = false;
                        for (auto& d : last_debuffs_) {
                            if (d == w) { found = true; break; }
                        }
                        if (!found) {
                            olog("⚠ 想要的Debuff未出现: " + w);
                            debuff_bad = true;
                        }
                    }
                }
            }

            if (stopped()) break;

            // 阶段二
            std::string selected_env = phase2();
            if (stopped()) break;

            if (selected_env.empty()) {
                olog("未能选择投资环境，终止");
                break;
            }

            // Debuff 不满足 → 退出重置
            if (debuff_bad) {
                olog("Debuff不满足条件，选了 [" + selected_env + "] 但仍退出重置");
                phase3_exit();
                continue;
            }

            // 判断是否目标
            bool is_target = false;
            for (auto& kw : config_.target_envs) {
                if (selected_env.find(kw) != std::string::npos) {
                    is_target = true;
                    break;
                }
            }

            if (is_target) {
                ostep("★ 目标达成: " + selected_env);
                olog("★ 目标达成: " + selected_env);
                // 通知
                std::string border(52, '=');
                std::cout << "\n" << border << "\n"
                          << "  找到目标投资环境：【" << selected_env << "】\n"
                          << "  请在游戏内手动继续后续操作。\n"
                          << border << "\n" << std::endl;
                break;
            } else {
                olog("非目标 [" + selected_env + "]，退出重置");
                phase3_exit();
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[bot] 异常: " << e.what() << std::endl;
    }

    clear_marks();
    if (overlay_) {
        overlay_->stop();
        overlay_.reset();
    }

    hotkey_running = false;
    if (hotkey_thread.joinable()) hotkey_thread.join();

    std::cout << "[bot] 自动化已停止。" << std::endl;
}
