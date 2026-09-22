# fixtures

`demo-vault` is the vault every screenshot and visual check uses. One note
exercises most of the syntax noteAppC renders, a second gives it something to
link to, embed and resolve.

```sh
./build/tools/noteAppC_shot fixtures/demo-vault fixtures/demo-vault/Welcome.md out.png dark 1240 800
```

It carries a `.obsidian/app.json` so the vault-config path is exercised too.
Keep it committed: a screenshot is only useful if the next person can take the
same one.
