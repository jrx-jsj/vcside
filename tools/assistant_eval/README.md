# Assistant evaluation

Measures how much the knowledge base helps a model: each question in `questions.json` is asked twice (as is, and with the entries the assistant would add) and the answer is checked for its key facts.

```sh
cmake --build build --target vcside_ground
python3 tools/assistant_eval/run.py --helper <llama-server> --model <model.gguf> --ground build/vcside_ground [--show] [--out results.json]
```

The pass check is a set of regular expressions per question, not a judge: it counts a wrong answer that happens to use the right words, and misses a right answer in other words. Read the answers (`--show`) before trusting a number. Add questions here when a user reports a wrong answer, and keep ones the knowledge base does not cover yet: those are the to-do list.
