function processItems(items) {
    try {                                  // try_catch
        for (const item of items) {        // loop (for_in_statement)
            if (item.valid) {              // if_branch
                return item;               // early_return
            } else {                       // else_branch
                continue;
            }
        }
    } catch (e) {                          // try_catch
        return null;                       // early_return
    }

    switch (items.length) {
        case 0:                            // switch_case
            return [];                     // early_return
        case 1:                            // switch_case
            break;
    }
    return items;                          // early_return
}

function loopTypes(n) {
    while (n > 0) {                        // loop
        n--;
    }
    do {                                   // loop
        n++;
    } while (n < 10);
    for (let i = 0; i < n; i++) {          // loop
        if (i === 5) return i;             // if_branch + early_return
    }
}
