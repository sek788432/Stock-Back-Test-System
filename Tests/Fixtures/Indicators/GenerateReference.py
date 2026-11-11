"""Generate synthetic indicator goldens with independent batch formulas.

Pass an output directory to regenerate build-local Bars.csv and Expected.csv. The fixed seed and
Decimal weighted-series/two-pass formulas do not call production indicators.
CSV names refer to the public enum names, not their ordinal encodings.
"""
from decimal import Decimal, getcontext
from pathlib import Path
import csv
import sys
import random
getcontext().prec = 60
D = Decimal

def smooth(values, n, alpha):
    # Closed finite weighted series, independently evaluated at each index.
    result = [None] * len(values)
    seed = sum(values[:n]) / D(n)
    for i in range(n - 1, len(values)):
        steps = i - n + 1
        result[i] = seed * ((1 - alpha) ** steps if steps else D(1)) + sum((alpha * ((1 - alpha) ** (i - j) if i != j else D(1)) * values[j] for j in range(n, i + 1)))
    return result

def oracle(definition, rows):
    kind, n, secondary, signal, out, field = definition
    inputs = [D(str(row[field + 1])) for row in rows]
    highs = [D(str(row[2])) for row in rows]
    lows = [D(str(row[3])) for row in rows]
    closes = [D(str(row[4])) for row in rows]
    volumes = [D(str(row[5])) for row in rows]
    count = len(rows)
    result = [None] * count
    tr = [max(highs[i] - lows[i], abs(highs[i] - closes[i - 1]), abs(lows[i] - closes[i - 1])) if i else highs[i] - lows[i] for i in range(count)]
    if kind == 1:
        return smooth(inputs, n, D(2) / D(n + 1))
    if kind == 3:
        changes = [inputs[i] - inputs[i - 1] for i in range(1, count)]
        gains = smooth([max(x, D(0)) for x in changes], n, D(1) / n)
        losses = smooth([max(-x, D(0)) for x in changes], n, D(1) / n)
        return [None] + [None if g is None else D(50) if g == l == 0 else D(100) if l == 0 else D(100) * g / (g + l) for g, l in zip(gains, losses)]
    if kind == 4:
        fast = smooth(inputs, n, D(2) / D(n + 1))
        slow = smooth(inputs, secondary, D(2) / D(secondary + 1))
        lines = [f - s if s is not None else None for f, s in zip(fast, slow)]
        signals = [None] * (secondary - 1) + smooth(lines[secondary - 1:], signal, D(2) / D(signal + 1))
        return lines if out == 0 else signals if out == 1 else [a - b if b is not None else None for a, b in zip(lines, signals)]
    if kind == 6:
        return smooth(tr, n, D(1) / n)
    if kind == 7:
        plus = [max(highs[i] - highs[i - 1], D(0)) if highs[i] - highs[i - 1] > lows[i - 1] - lows[i] else D(0) for i in range(1, count)]
        minus = [max(lows[i - 1] - lows[i], D(0)) if lows[i - 1] - lows[i] > highs[i] - highs[i - 1] else D(0) for i in range(1, count)]
        ranges = smooth(tr[1:], n, D(1) / n)
        plus = smooth(plus, n, D(1) / n)
        minus = smooth(minus, n, D(1) / n)
        p = [D(100) * a / t if a is not None and t else None for a, t in zip(plus, ranges)]
        m = [D(100) * a / t if a is not None and t else None for a, t in zip(minus, ranges)]
        if out == 8:
            return [None] + p
        if out == 9:
            return [None] + m
        dx = [D(100) * abs(a - b) / (a + b) if a + b else D(0) for a, b in zip(p[n - 1:], m[n - 1:])]
        return [None] * n + smooth(dx, n, D(1) / n)
    if kind == 11:
        return [sum((volumes[j] if closes[j] > closes[j - 1] else -volumes[j] if closes[j] < closes[j - 1] else D(0) for j in range(1, i + 1))) for i in range(count)]
    if kind == 14:
        return tr
    if kind == 15:
        return inputs
    k = []
    for i in range(count):
        window = inputs[max(0, i - n + 1):i + 1]
        if kind in (12, 13):
            if i >= n:
                result[i] = D(100) * (inputs[i] - inputs[i - n]) / inputs[i - n] if kind == 12 and inputs[i - n] else None if kind == 12 else inputs[i] - inputs[i - n]
            continue
        if i < n - 1:
            continue
        high = max(highs[i - n + 1:i + 1])
        low = min(lows[i - n + 1:i + 1])
        if kind == 0:
            result[i] = sum(window) / n
        if kind == 2:
            result[i] = sum((x * w for w, x in enumerate(window, 1))) / D(n * (n + 1) // 2)
        if kind == 5:
            mean = sum(window) / n
            sd = (sum(((x - mean) ** 2 for x in window)) / n).sqrt()
            upper = mean + 2 * sd
            lower = mean - 2 * sd
            result[i] = {3: upper, 4: mean, 5: lower, 6: (upper - lower) / mean if mean else None, 7: (inputs[i] - lower) / (upper - lower) if upper != lower else D('.5')}[out]
        if kind == 8:
            k.append(D(100) * (closes[i] - low) / (high - low) if high != low else D(50))
            sn = signal or 3
            result[i] = k[-1] if out == 10 else sum(k[-sn:]) / sn if len(k) >= sn else None
        if kind == 9:
            result[i] = {3: high, 4: (high + low) / 2, 5: low}[out]
        if kind == 10:
            v = volumes[i - n + 1:i + 1]
            total = sum(v)
            result[i] = sum((x * y for x, y in zip(window, v))) / total if total else None
    return result

def main():
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    kinds = ['sma', 'ema', 'wma', 'rsi', 'macd', 'bollingerBands', 'atr', 'adx', 'stochastic', 'donchian', 'vwap', 'obv', 'roc', 'momentum', 'trueRange', 'barField']
    output_names = ['value', 'signal', 'histogram', 'upper', 'middle', 'lower', 'width', 'percentB', 'positiveDirectionalIndex', 'negativeDirectionalIndex', 'percentK', 'percentD']
    field_names = ['open', 'high', 'low', 'close', 'volume']
    outputs = {4: [0, 1, 2], 5: [3, 4, 5, 6, 7], 7: [0, 8, 9], 8: [10, 11], 9: [3, 4, 5]}
    rng = random.Random(4208)
    rows = []
    for i in range(140):
        close = rng.randint(500, 2000) / 10
        opening = close + rng.randint(-10, 10) / 10
        rows.append([i * 3600000 + i // 19 * 86400000, opening, max(opening, close) + rng.randint(0, 30) / 10, min(opening, close) - rng.randint(0, 30) / 10, close, rng.randint(0, 10000)])
    with (root / 'Bars.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(['timestamp_ms', 'open', 'high', 'low', 'close', 'volume'])
        writer.writerows(rows)
    with (root / 'Expected.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(['kind', 'period', 'secondaryPeriod', 'signalPeriod', 'output', 'field'] + [f'bar{i}' for i in range(len(rows))])
        for period in (1, 2, 3, 14):
            for kind, name in enumerate(kinds):
                for output in outputs.get(kind, [0]):
                    for field in range(5) if kind == 15 else [3]:
                        definition = [kind, period, period + 2, 3, output, field]
                        values = oracle(definition, rows)
                        writer.writerow([name, period, period + 2, 3, output_names[output], field_names[field]] + ['none' if value is None else format(value, '.17g') for value in values])
if __name__ == '__main__':
    main()
