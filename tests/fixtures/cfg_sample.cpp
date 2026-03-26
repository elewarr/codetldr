#include <vector>
#include <stdexcept>

int analyze(const std::vector<int>& data) {
    int result = 0;
    try {                                  /* try_catch */
        for (const auto& val : data) {     /* loop (for_range_loop) */
            if (val > 0) {                 /* if_branch */
                result += val;
            } else {                       /* else_branch */
                return -1;                 /* early_return */
            }
        }
    } catch (const std::exception& e) {   /* try_catch */
        return -2;                         /* early_return */
    }
    return result;                         /* early_return */
}
