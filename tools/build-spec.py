"""Render the Japanese specification. Requires reportlab and a Japanese TTF/TTC."""
import argparse
import re
from html import escape
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import letter
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, Table, TableStyle, PageBreak

parser = argparse.ArgumentParser()
parser.add_argument('--font', required=True)
parser.add_argument('--out', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / 'docs/SERVER-MODEM-SPEC.md'
pdfmetrics.registerFont(TTFont('Japanese', args.font, subfontIndex=0))
normal = ParagraphStyle('Normal', fontName='Japanese', fontSize=11, leading=17,
                        spaceAfter=9, wordWrap='CJK')
title = ParagraphStyle('Title', parent=normal, fontSize=22, leading=30, spaceAfter=20)
heading = ParagraphStyle('Heading', parent=normal, fontSize=16, leading=23,
                         spaceBefore=20, spaceAfter=12, keepWithNext=True)
cell = ParagraphStyle('Cell', parent=normal, fontSize=9.5, leading=14, spaceAfter=0)
headercell = ParagraphStyle('HeaderCell', parent=cell, textColor=colors.white)
code = ParagraphStyle('Code', parent=normal, fontSize=9, leading=13, spaceAfter=10)
width = letter[0] - 96

def text(value):
    value = re.sub(r'`([^`]+)`', r'\1', value)
    return escape(value)

def table(lines):
    rows = []
    for ln in lines:
        values = [v.strip() for v in ln.strip().strip('|').split('|')]
        if all(re.fullmatch(r':?-+:?', v) for v in values):
            continue
        rows.append(values)
    cols = len(rows[0])
    if cols == 2:
        ratios = [.28, .72]
    elif cols == 4:
        ratios = [.21, .13, .19, .47]
    else:
        # Keep the first field compact while leaving descriptions usable.
        ratios = [.21, .24, .55]
    widths = [width * r for r in ratios]
    data = [[Paragraph(text(v), headercell if i == 0 else cell) for v in row]
            for i, row in enumerate(rows)]
    tab = Table(data, colWidths=widths, repeatRows=1, hAlign='LEFT')
    tab.setStyle(TableStyle([
        ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#333333')),
        ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, colors.HexColor('#F5F5F5')]),
        ('GRID', (0, 0), (-1, -1), .5, colors.HexColor('#D9D9D9')),
        ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
        ('LEFTPADDING', (0, 0), (-1, -1), 8),
        ('RIGHTPADDING', (0, 0), (-1, -1), 8),
        ('TOPPADDING', (0, 0), (-1, -1), 6),
        ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
    ]))
    return [tab, Spacer(1, 12)]

lines = source.read_text(encoding='utf-8').splitlines()
story = []
i = 0
while i < len(lines):
    ln = lines[i]
    if not ln.strip():
        i += 1
        continue
    if ln.startswith('# '):
        story.append(Paragraph(text(ln[2:]), title))
    elif ln.startswith('## '):
        if ln.startswith('## 1 '):
            story.append(PageBreak())
        story.append(Paragraph(text(ln[3:]), heading))
    elif ln.startswith('|'):
        group = []
        while i < len(lines) and lines[i].startswith('|'):
            group.append(lines[i])
            i += 1
        story.extend(table(group))
        continue
    elif ln.startswith('```'):
        i += 1
        group = []
        while i < len(lines) and not lines[i].startswith('```'):
            group.append(lines[i])
            i += 1
        story.append(Paragraph('<br/>'.join(text(s).replace(' ', '&nbsp;') for s in group), code))
    elif ln.startswith('- '):
        story.append(Paragraph('・ ' + text(ln[2:]), normal))
    else:
        story.append(Paragraph(text(ln), normal))
    i += 1

def footer(canvas, doc):
    canvas.saveState()
    canvas.setFont('Japanese', 8)
    canvas.setFillColor(colors.black)
    canvas.drawString(48, 25, 'Saturn XBAND v0.1.0 仕様書 第1版')
    canvas.drawRightString(letter[0]-48, 25, str(doc.page))
    canvas.restoreState()

out = Path(args.out).resolve()
out.parent.mkdir(parents=True, exist_ok=True)
document = SimpleDocTemplate(str(out), pagesize=letter, leftMargin=48, rightMargin=48,
                             topMargin=43, bottomMargin=45,
                             title='Saturn XBAND サーバーとモデムの仕様書', author='Saturn XBAND project')
document.build(story, onFirstPage=footer, onLaterPages=footer)
print(out)
