"""Build the defense report from traceable source/Git/verification evidence.

Requires ReportLab and pypdf from the Codex bundled runtime. Does not run tests.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_LEFT
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import SimpleDocTemplate, Paragraph, Spacer, PageBreak, KeepTogether, Image
from pypdf import PdfReader
from reportlab.pdfgen import canvas
from reportlab.lib.utils import ImageReader

REPO = Path(__file__).resolve().parents[1]
ROOT = REPO / 'docs/lab2/defense'


def load(name: str, default=None):
    path = ROOT / name
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else default


def source(ref):
    return f"{ref['path']}:{ref['line']}"


def generate_markdown():
    facts = load('implementation-evidence.json')
    git = load('git-snapshot.json')
    quality = load('verification.json', {'status': 'Проверки ещё выполняются. Финальная версия отчёта требует verification.json.'})
    content = load('content.json')
    branch = git['branch']
    counts = git['counts']
    lines = [
        '# TurboGameEngine: технический отчёт к защите ЛР 2',
        '', 'Дата: 16.10.2026. Основание: ТЗ «Скриптинг», текущий код, тесты и зафиксированные локальные прогоны.',
        '', '## 1. Результат и граница работы',
        '', 'В C++20 движок встроена Luau VM. Игровые решения выполняет скриптовая механика Collectathon. Префабы и поля связаны с ECS и Inspector; hot reload сохраняет состояние поведения. Настоящий React Luau работает через собственный retained host и native renderer. Подготовленная презентация тоже является Luau/React-проектом движка.',
        '', 'Документ охватывает весь текущий результат, включая незакоммиченные подсистемы. Это не полный Roblox runtime. Наличие реализации и факт выполнения теста разделены: матрица содержит исходники, сводка проверок - результаты запусков.',
        '', '## 2. Архитектура и поток данных',
        '', 'Application создаёт платформу, jobs, renderer и state stack. GameplayState владеет миром и связывает с ним ScriptingSystem. Одна VM обслуживает экземпляры поведения мира. ScriptComponent сериализует путь модуля, enabled и fields; VM references остаются вне ECS.',
        '', 'На шаге симуляции движок вызывает поведение. Engine/Input позволяют безопасно работать с сущностями, Transform, физикой, prefab spawn, запросами и статусом. Сущности идентифицируются handle с поколением; destroy из callback откладывается до безопасной границы, spawnPrefab создаёт сущность сразу на потоке владельца. Lua-вызовы и ECS apply выполняются на owner thread.',
        '', 'При изменении файлов watcher ограниченно читает и сравнивает содержимое. Jobs читают/компилируют модули; поток владельца применяет результат и зависимые require. Успех сохраняет self.state и вызывает OnReload; неудача оставляет previous-good код. Destroy/Stop очищают экземпляры и UI, shutdown ждёт jobs и закрывает VM.',
        '', '## 3. Сопоставление с обязательными требованиями',
    ]
    labels = {'implemented': 'Реализовано', 'implemented_with_limits': 'Реализовано с ограничениями',
              'partial': 'Частично', 'not_implemented': 'Не реализовано',
              'known_runtime_failure_under_investigation': 'Исходный дефект зафиксирован; актуальный итог в проверках'}
    for item in facts['requirements']:
        if item.get('kind') != 'mandatory':
            continue
        lines += ['', f"### {item['id']}: {item['requirement']}", '',
                  f"Статус исходников: {labels.get(item['implementation_status'], item['implementation_status'])}.",
                  '', item['actual_behavior'],
                  '', 'Доказательства: ' + '; '.join(source(e) for e in item.get('evidence', [])) + '.',
                  '', 'Пределы: ' + ' '.join(item.get('limits', []))]
    lines += ['', '## 4. Полный обзор результата рабочего дерева', '',
              f"Ниже - все {len(facts['feature_inventory'])} группы реализации и материалов из аудита. Файл git-snapshot.json содержит полный пофайловый перечень; vendored и служебные файлы не выдаются за самостоятельные пользовательские функции."]
    for item in facts['feature_inventory']:
        lines += ['', f"### {item['title']}", '', item['actual'],
                  '', 'Исходники: ' + '; '.join(source(e) for e in item.get('evidence', [])[:3]) + '.',
                  '', 'Ограничения: ' + ' '.join(item.get('limits', []))]
    lines += ['', '## 5. Качество и дополнительные пункты ТЗ', '']
    for item in facts['requirements']:
        if item.get('kind') == 'mandatory':
            continue
        if item['id'] == '3.stability':
            lines += ['- 3.stability: обнаруженный дефект отложенного уничтожения исправлен. Повторный Windows CTest 19/19 PASS; Linux ASan/LSan/UBSan 11/11 с 240 lifecycle-циклами; Windows native 300 секунд без ошибок Luau. Подробности и пределы - раздел 6 и memory-report.md. Все сценарии движка/GPU этим не покрыты.']
            continue
        lines += [f"- {item['id']}: {labels.get(item['implementation_status'], item['implementation_status'])}. {item['actual_behavior']} {' '.join(item.get('limits', []))}"]
    lines += ['', '## 6. Проверки, память и стабильность', '',
              'Актуальная машиночитаемая сводка: verification.json. Подробности и сырые логи: memory-report.md, memory-linux/, memory-windows-*/ и evidence/.', '',
              'Базовый Windows CTest: 19/19 PASS, 35.96 секунды. Это результат до исправления дефекта из нагрузочного прогона; повторные проверки отражены в итоговой сводке.', '',
              'Фактический CLion Profile app на Windows/MSVC вернул: «Perf profiler is not available for selected toolchain». Это ограничение выбранного инструментария. Снимок сохранён как evidence/clion-profiler-unavailable.jpg. CPU profile не равен проверке утечек.', '',
              'Первый 300-секундный Release/D3D12 прогон повторял создание/уничтожение Collectathon и React HUD и перезагрузку скрипта. Выявлено исчерпание 64 MiB Luau. Исходный exit=0 не означает успех: runtime содержал ошибки. Эти отрицательные данные сохранены, а не заменены новым успешным логом.', '',
              'Счётчики Private Bytes/Working Set/handles показывают поведение процесса, но не называют утечки. ASan/UBSan/LSan проверяют инструментированные CPU-пути. LSan при lua_close не обнаружит как leak всё, что ещё было достижимо до закрытия VM; для этого нужен same-VM lifecycle stress. GPU/драйвер и все пользовательские сценарии этим не покрыты.', '',
              *quality.get('reportParagraphs', ['Итоговые проверки ещё выполняются; текущая сводка - verification.json.']), '',
              '## 7. Git: что ещё не закоммичено и не отправлено', '',
              f"Снимок: {git['generated_at_utc']}. Ветка {branch['name']}; HEAD {branch['head']}. Upstream {branch['upstream']}; ahead={branch['ahead']}, behind={branch['behind']} по локальным tracking refs. Fetch не выполнялся; состояние remote-сервера не проверялось.", '',
              f"Изменённых путей: {counts['changed_paths']}; tracked без staged: {counts['unstaged_tracked_paths']}; untracked: {counts['untracked_paths']}; staged: {counts['staged_paths']}. По этому локальному сравнению неотправленных коммитов нет. Несохранённые в Git изменения существуют независимо от ahead=0.", '',
              'Полный перечень - git-snapshot.json / files. Он включает изменения, новые файлы и предсуществующее удаление reports/5.pdf. Удаление не выполнялось этой работой. Игнорируемые REA-исследования/reference fonts/личные настройки перечислены отдельно; временные зависимости и копии fixtures не являются новой функциональностью.', '',
              'Существующая закоммиченная основа: ' + '; '.join(x['commit'] + ' ' + x['subject'] for x in facts['committed_foundation_not_new_lab2']) + '. Она используется, но не приписывается целиком новой ЛР 2.', '',
              '## 8. Поставка и демонстрация', '',
              'Luau VM/Compiler статически связаны. В поставку входят app и необходимые DLL, встроенные assets, React-пакеты и используемые инструменты редактора. Studio/Python/npm/Codex не нужны для запуска игры. Проверка на чистой пользовательской машине не выполнялась.', '',
              'Открыть demos/lab2-defense/Lab2Defense.tgeproj через Projects, затем Play. Игра содержит восемь тем и три задания: квиз, реальные prefab/Transform и проверку настоящего OnReload. Кнопка смены темы не считается reload. Подробные действия - README демонстрации.', '',
              'Сценарий доклада, 180-секундной демонстрации Collectathon и ответы - defense-notes.md. Резервные слайды имеют speaker notes с источниками.', '',
              '## 9. Явно не заявляется', '']
    lines += ['- ' + x for x in content['appendix']['limits']]
    lines += ['- Нет object-reference/enum field schema, bind generator, полного gameplay subscribe/unsubscribe bus, script raycast/sweep или savegame всех VM состояний.',
              '- Check проверяет компиляцию/синтаксис, а не заменяет полноценный typechecker/LSP.',
              '- Смена модели в Codex-каталоге не доказывает успешный платный inference; в проверках использованы протокол/локальные fixtures.', '',
              '## 10. Источники и воспроизводимость', '',
              'implementation-evidence.json хранит точные file:line/anchor/excerpt; номера строк относятся к моменту аудита. git-snapshot.json хранит состояние файлов на момент снимка. verification.json связывает итог с конкретными логами и бинарниками.', '',
              'Документация JetBrains: https://www.jetbrains.com/help/clion/cpu-profiler.html и https://www.jetbrains.com/help/clion/memory-profiling-with-valgrind.html (проверены 16.10.2026). CPU profiler документирован для Linux/macOS; Memcheck на Windows интегрируется через WSL. Онлайн-справка может быть новее установленного CLion 2026.1; локальная GUI-ошибка является самостоятельным доказательством текущей конфигурации.', '',
              'Авторские материалы и проверки не выполняют commit/push, не меняют глобальную IDE-конфигурацию и не переписывают пользовательские проекты.']
    text = '\n'.join(lines) + '\n'
    (ROOT / 'technical-report.md').write_text(text, encoding='utf-8')
    return text


def pdf_text(text):
    return text.replace('\u2011', '-').replace('\u2013', '-').replace('\u2014', '-').replace('\u202f', ' ')


def render_report(markdown):
    pdfmetrics.registerFont(TTFont('Arial', 'C:/Windows/Fonts/arial.ttf'))
    pdfmetrics.registerFont(TTFont('ArialBold', 'C:/Windows/Fonts/arialbd.ttf'))
    pdfmetrics.registerFontFamily('Arial', normal='Arial', bold='ArialBold', italic='Arial', boldItalic='ArialBold')
    styles = {
        'body': ParagraphStyle('Body', fontName='Arial', fontSize=10.3, leading=15.4, textColor=colors.HexColor('#243248'), spaceAfter=8, splitLongWords=True),
        'title': ParagraphStyle('Title', fontName='ArialBold', fontSize=28, leading=34, spaceAfter=22, textColor=colors.HexColor('#11243A')),
        'h2': ParagraphStyle('Section', fontName='ArialBold', fontSize=17, leading=22, spaceBefore=17, spaceAfter=10, keepWithNext=True, textColor=colors.HexColor('#146B70')),
        'h3': ParagraphStyle('Subsection', fontName='ArialBold', fontSize=12, leading=17, spaceBefore=9, spaceAfter=7, keepWithNext=True),
        'bullet': ParagraphStyle('Bullet', fontName='Arial', fontSize=10, leading=14.5, leftIndent=10, spaceAfter=6),
    }
    story=[]
    subsection=[]
    def flush_subsection():
        if subsection:
            story.append(KeepTogether(list(subsection)))
            subsection.clear()
    for raw in pdf_text(markdown).split('\n'):
        if not raw.strip():
            continue
        if raw.startswith('# '):
            flush_subsection()
            style,value=styles['title'],raw[2:]
        elif raw.startswith('## '):
            flush_subsection()
            if raw.startswith('## 3.'):
                story.append(PageBreak())
            style,value=styles['h2'],raw[3:]
        elif raw.startswith('### '):
            flush_subsection()
            style,value=styles['h3'],raw[4:]
        elif raw.startswith('- '):
            style,value=styles['bullet'],'• '+raw[2:]
        else:
            style,value=styles['body'],raw
        # Evidence JSON is kept readable as plain paragraphs, not raw syntax lines.
        if value.strip() in ('{','}','},','[',']','],'):
            continue
        value=escape(value)
        value=re.sub(r'`([^`]+)`',r'<b>\1</b>',value)
        paragraph=Paragraph(value,style)
        if raw.startswith('### ') or subsection:
            subsection.append(paragraph)
        else:
            story.append(paragraph)
    flush_subsection()
    path=ROOT/'output/TurboGameEngine-Lab2-report.pdf'
    def footer(canvas,doc):
        canvas.saveState()
        canvas.setStrokeColor(colors.HexColor('#DCE4EB'))
        canvas.line(20*mm,17*mm,190*mm,17*mm)
        canvas.setFont('Arial',8)
        canvas.setFillColor(colors.HexColor('#617288'))
        canvas.drawString(20*mm,11*mm,'TurboGameEngine | ЛР 2 | 16.10.2026')
        canvas.drawRightString(190*mm,11*mm,str(doc.page))
        canvas.restoreState()
    SimpleDocTemplate(str(path),pagesize=(210*mm,297*mm),leftMargin=20*mm,rightMargin=20*mm,
                      topMargin=19*mm,bottomMargin=23*mm,title='TurboGameEngine: ЛР 2, технический отчёт',
                      author='TurboGameEngine').build(story,onFirstPage=footer,onLaterPages=footer)
    reader=PdfReader(path)
    assert len(reader.pages)>0
    assert 'TurboGameEngine' in ''.join(p.extract_text() or '' for p in reader.pages)
    print(json.dumps({'pdf':str(path),'pages':len(reader.pages)},ensure_ascii=False))


def render_slides_pdf():
    renders = sorted((ROOT/'.build/final-deck/renders').glob('slide-*.png'))
    if not renders:
        return
    output = ROOT/'output/TurboGameEngine-Lab2-slides.pdf'
    page = canvas.Canvas(str(output), pagesize=(960, 540))
    page.setTitle('TurboGameEngine: ЛР 2, резервные слайды')
    for image in renders:
        page.drawImage(ImageReader(str(image)),0,0,width=960,height=540)
        page.showPage()
    page.save()
    assert len(PdfReader(output).pages) == len(renders)


if __name__ == '__main__':
    render_report(generate_markdown())
    render_slides_pdf()
