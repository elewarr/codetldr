def analyze(data, threshold):
    """Fixture: covers all 6 CFG node types."""
    results = []
    try:                            # try_catch
        for item in data:           # loop
            if item > threshold:    # if_branch
                results.append(item)
            elif item == 0:         # if_branch (elif)
                return []           # early_return
            else:                   # else_branch
                pass
    except ValueError:              # try_catch
        return None                 # early_return
    return results                  # early_return


def simple_branch(x):
    if x > 0:                       # if_branch, depth 0
        if x > 10:                  # if_branch, depth 1
            return x                # early_return, depth 2
    return 0                        # early_return, depth 0
