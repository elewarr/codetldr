#include <stdio.h>

int process(int* data, int len) {
    int sum = 0;
    for (int i = 0; i < len; i++) {       /* loop */
        if (data[i] > 0) {                /* if_branch */
            sum += data[i];
        } else {                          /* else_branch */
            return -1;                    /* early_return */
        }
    }
    switch (sum) {                        /* (switch captured via case children) */
        case 0:                           /* switch_case */
            return 0;                     /* early_return */
        case 1:                           /* switch_case */
            break;
    }
    return sum;                           /* early_return */
}

void simple(int x) {
    if (x > 0) {                          /* if_branch */
        while (x > 0) {                   /* loop */
            x--;
        }
    }
}
