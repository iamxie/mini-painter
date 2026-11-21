"""重造 HEIC 測試檔（src/tests/fixtures/heic-*.heic）。

用法：uv run --with pillow --with pillow-heif scripts/make_heic_fixtures.py
pillow-heif 的 wheel 帶 libheif 與 x265 編碼器；只在這裡造檔，程式不連結它們。
來源與內容寫在 src/tests/fixtures/heic-SOURCES.txt，改了這裡要一起改。
"""
from pathlib import Path

from PIL import Image
import pillow_heif

FIXTURES = Path(__file__).resolve().parent.parent / "src" / "tests" / "fixtures"
W, H, MARK = 64, 48, 8  # 寬、高、左上角紅塊的邊長


def pattern() -> Image.Image:
    """灰底，左上角 MARK x MARK 的紅塊：看得出方向。"""
    img = Image.new("RGB", (W, H), (128, 128, 128))
    img.paste((255, 0, 0), (0, 0, MARK, MARK))
    return img


def main() -> None:
    pillow_heif.register_heif_opener()
    FIXTURES.mkdir(parents=True, exist_ok=True)

    # HEVC，EXIF 帶 Make / Model 與方向 6。libheif 把方向寫成 irot（存的是轉過的），EXIF 的方向欄位留著。
    exif = Image.Exif()
    exif[0x010F] = "mpainter test"  # Make
    exif[0x0110] = "heic fixture"   # Model
    exif[0x0112] = 6                # Orientation：順時針轉 90 度顯示
    pattern().save(FIXTURES / "heic-exif-orient6.heic", format="HEIF", quality=90, exif=exif.tobytes())

    # 副檔名 .heic、內容是 AVIF（AV1）。Pillow 11.3 起內建 AVIF 編碼。
    pattern().save(FIXTURES / "heic-avif.heic", format="AVIF", quality=90)

    for p in sorted(FIXTURES.glob("heic-*.heic")):
        print(p.name, p.stat().st_size, p.read_bytes()[:32])


if __name__ == "__main__":
    main()
