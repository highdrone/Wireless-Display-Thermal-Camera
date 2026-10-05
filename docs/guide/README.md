# User guide source

`guide.html` is the source of [`../user-guide.pdf`](../user-guide.pdf): one
print-styled HTML page per PDF page (US Letter). It uses the project's
simulated screenshots and the Space Grotesk, Inter and JetBrains Mono fonts
from Google Fonts, so rendering needs an Internet connection.

To regenerate the PDF with Google Chrome (macOS shown; on Linux use
`google-chrome` or `chromium`), from the repository root:

```sh
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
  --no-pdf-header-footer --virtual-time-budget=5000 \
  --print-to-pdf="$PWD/docs/user-guide.pdf" "file://$PWD/docs/guide/guide.html"
```

Check every page afterwards: each `<section class="page">` must fit on one
page, and the footer page numbers and section numbers are written by hand.
