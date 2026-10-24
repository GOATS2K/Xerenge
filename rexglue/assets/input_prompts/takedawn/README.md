# PlayStation prompts after Takedawn

The PlayStation set in `../replace/playstation/` (the menu atlas
`C3E82000DB0002CD.png` and the race's icons `FB913238D525726F.png`,
`54EFE1A4D2A2E52C.png`) is drawn from these SVGs, rendered with
`rsvg-convert`. The shapes, colours and stroke letters are those of
[Takedawn](https://github.com/xenophobentx/takedawn)'s
`assets/glyphs/playstation` (MIT, `LICENSE.takedawn`), laid out at the places
where Burnout Revenge keeps its prompts; the right stick and the d-pad are
drawn here in the same manner.

    rsvg-convert atlas.svg -o ../replace/playstation/C3E82000DB0002CD.png
    rsvg-convert race_cross.svg -o ../replace/playstation/FB913238D525726F.png
    rsvg-convert race_circle.svg -o ../replace/playstation/54EFE1A4D2A2E52C.png
