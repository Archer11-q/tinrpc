#pragma once

#include <cstdint>

namespace game {

// ============================================================
// EloCalculator — ELO 评分计算器
//
// 用于匹配系统的技能评分。标准 ELO 公式：
//   E_A = 1 / (1 + 10^((R_B - R_A) / 400))
//   R_A' = R_A + K * (S_A - E_A)
//
// K 因子控制分数变化幅度（K 越大，单局分数变化越大）。
// ============================================================
class EloCalculator {
public:
    // k: K 因子（默认 32，标准比赛用值）
    explicit EloCalculator(double k = 32.0);

    // 计算 A 对 B 的期望胜率（0.0 ~ 1.0）
    double CalcExpected(double rating_a, double rating_b) const;

    // 更新 A 的评分
    // rating_a / rating_b: 赛前双方评分
    // result_a: A 的实际结果（1.0 = 胜, 0.0 = 负, 0.5 = 平）
    // 返回 A 的新评分
    double UpdateRating(double rating_a, double rating_b, double result_a) const;

    double K() const {
        return k_;
    }

private:
    double k_;
};

} // namespace game
