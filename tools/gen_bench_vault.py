#!/usr/bin/env python3
"""Generates a synthetic vault for timing the indexer.

    python3 tools/gen_bench_vault.py /tmp/bench-vault 20000
    build/tools/noteAppC-index --fresh /tmp/bench-vault

Notes are spread over five folders with a realistic link density (two to eight
wikilinks each, so the name table and resolution do real work) and carry
frontmatter, tags, a list, a task, a table and a fenced code block. The seed is
fixed, so two runs produce the same vault and two timings compare.

The budget is 20 000 notes in under 5 s. That was 39.8 s until the
full-text table stopped being scanned per note; see the commit that says so.
"""

import os, random, sys, textwrap
random.seed(7)
root = sys.argv[1]
N = int(sys.argv[2])
os.makedirs(root + "/.obsidian", exist_ok=True)
open(root + "/.obsidian/app.json", "w").write('{"userIgnoreFilters": []}\n')
folders = ["areas", "projects", "daily", "refs", "people"]
names = ["{}/Note {:05d}".format(folders[i % len(folders)], i) for i in range(N)]
for d in folders:
    os.makedirs(os.path.join(root, d), exist_ok=True)
tmpl = textwrap.dedent("""\
    ---
    title: {title}
    tags: [{tag}]
    created: 2026-09-{day:02d}
    ---

    # {title}

    Some prose with **bold**, *italic* and `code`. {tags}

    {links}

    - a list item
    - [ ] a task
    - [x] a done task

    | a | b |
    | - | - |
    | 1 | 2 |

    ```python
    def f(x):
        return x * {i}
    ```
    """)
for i, name in enumerate(names):
    links = [names[random.randrange(N)].split("/")[-1] for _ in range(random.randint(2, 8))]
    tags = " ".join("#" + random.choice(["work", "idea", "status/active", "status/done", "read"])
                    for _ in range(2))
    body = tmpl.format(title=name.split("/")[-1], tag=random.choice(["work", "idea", "read"]),
                       day=random.randint(1, 28), tags=tags, i=i,
                       links="\n".join("- See [[{}]]".format(l) for l in links))
    open(os.path.join(root, name + ".md"), "w").write(body)
print("wrote", N, "notes")
