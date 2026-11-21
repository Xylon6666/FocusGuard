"""生成 app.ico（纯 Python，无第三方依赖）。

图形：蓝色圆盘 + 白色中心点，视觉上等同"聚焦/靶心"。
32bpp BGRA + AND 掩码，支持多尺寸，抗锯齿用 4x 超采样。
"""
import struct, os

SIZES = [16, 20, 24, 32, 40, 48, 64]
SS = 4  # 超采样倍数


def lerp(a, b, t):
    return a + (b - a) * t


def shade(r):
    """按半径返回颜色（0..1 归一化半径）。"""
    if r <= 0.135:
        return (255, 255, 255)          # 中心白点
    if r <= 0.40:
        t = (r - 0.135) / (0.40 - 0.135)
        # 上部亮蓝 -> 下部深蓝
        top = (76, 134, 240)
        bot = (42, 85, 200)
        return tuple(int(lerp(top[i], bot[i], t)) for i in range(3))
    return (32, 63, 156)                # 边缘深蓝环


def render(size):
    px = []
    for y in range(size):
        row = []
        for x in range(size):
            acc_a = 0.0
            sum_r = sum_g = sum_b = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    # 以像素中心为原点，半径归一化到 1
                    fx = (x + (sx + 0.5) / SS) / size * 2.0 - 1.0
                    fy = (y + (sy + 0.5) / SS) / size * 2.0 - 1.0
                    d = (fx * fx + fy * fy) ** 0.5
                    if d <= 0.93:
                        acc_a += 1.0
                        c = shade(d / 0.93)
                        sum_r += c[0]
                        sum_g += c[1]
                        sum_b += c[2]
            n = SS * SS
            a = acc_a / n
            if acc_a > 0:
                rgb = (int(sum_r / acc_a), int(sum_g / acc_a), int(sum_b / acc_a))
            else:
                rgb = (0, 0, 0)
            row.append((rgb[2], rgb[1], rgb[0], int(a * 255)))  # BGRA
        px.append(row)
    return px


def ico_bytes(images):
    out = bytearray()
    out += struct.pack('<HHH', 0, 1, len(images))

    offset = 6 + 16 * len(images)
    payloads = []
    for size, rows in images:
        # XOR 位图：BGRA，自下而上
        xor = bytearray()
        for y in range(size - 1, -1, -1):
            for x in range(size):
                xor += bytes(rows[y][x])
        # AND 掩码：全 0（不透明由 alpha 决定），行按 4 字节对齐
        mask_row = ((size + 31) // 32) * 4
        mask = bytes(mask_row * size)

        bih = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0,
                          len(xor), 0, 0, 0, 0)
        data = bih + bytes(xor) + mask
        payloads.append((size, data))

    for size, data in payloads:
        w = 0 if size == 256 else size
        out += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(data), offset)
        offset += len(data)

    for _, data in payloads:
        out += data
    return bytes(out)


def main():
    images = [(s, render(s)) for s in SIZES]
    here = os.path.dirname(os.path.abspath(__file__))
    target = os.path.join(here, '..', 'src', 'app.ico')
    with open(target, 'wb') as f:
        f.write(ico_bytes(images))
    print('written:', os.path.abspath(target), os.path.getsize(target), 'bytes')


if __name__ == '__main__':
    main()
