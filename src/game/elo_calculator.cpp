#include "game/elo_calculator.h"

#include <cmath>

namespace game {

EloCalculator::EloCalculator(double k) : k_(k) {
}

double EloCalculator::CalcExpected(double rating_a, double rating_b) const {
    // E_A = 1 / (1 + 10^((R_B - R_A) / 400))
    double exponent = (rating_b - rating_a) / 400.0;
    return 1.0 / (1.0 + std::pow(10.0, exponent));
}

double EloCalculator::UpdateRating(double rating_a, double rating_b, double result_a) const {
    // R_A' = R_A + K * (S_A - E_A)
    double expected_a = CalcExpected(rating_a, rating_b);
    return rating_a + k_ * (result_a - expected_a);
}

} // namespace game
