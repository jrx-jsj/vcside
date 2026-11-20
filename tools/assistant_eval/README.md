# Assistant evaluation

Measures how much the knowledge base helps a model: each question in `questions.json` is asked twice (as is, and with the entries the assistant would add) and the answer is checked for its key facts.

```sh
cmake --build build --target vcside_ground
python3 tools/assistant_eval/run.py --helper <llama-server> --model <model.gguf> --ground build/vcside_ground [--show] [--out results.json]
```

The pass check is a set of regular expressions per question, not a judge: it counts a wrong answer that happens to use the right words, and misses a right answer in other words. Read the answers (`--show`) before trusting a number. Add questions here when a user reports a wrong answer, and keep ones the knowledge base does not cover yet: those are the to-do list.

## The agent loop

`agent_run.py` plays the panel for the agent (tools and edits, docs/design-assistant.md, phases 3 and 4): it uses the real tools through `vcside_tool`, feeds each result back to the model and accepts every proposed edit as a click on *Apply* would. Eight small tasks on a throw-away DASM project (find a value, read a file and count, look up a register, change a colour, describe a sprite and a song, refuse to read `.env`).

```sh
cmake --build build --target vcside_tool
python3 tools/assistant_eval/agent_run.py --helper <llama-server> --model <model.gguf> --tool build/vcside_tool [--show]
```

Tasks are few and the checks are regular expressions: use it to see whether a model can use the tools at all, not to rank models.

## The guided procedure (`guided_run.py`)

Measures the procedure that leads a small local model to a change (docs/design-assistant.md, "The guided procedure"):

    build/llm-helper/bin/llama-server -m <model.gguf> --port 8795 -c 16384 &
    python3 tools/assistant_eval/guided_run.py --url http://127.0.0.1:8795/v1 [--osito <a copy of a real DASM game>] [--runs 2] [--json out.json]

It needs `build/vcside_guided` and `build/vcside_new`. Each task of `guided_tasks.json` runs on a fresh copy of a project and passes when the
proposal assembles and the lines it adds have what the task asks for (regular expressions on the code). `--only <id>` runs one task.
