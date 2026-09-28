"""Cross-encoder reranking on ov.Core with explicit query and document pairs.

GenAI's TextRerankPipeline crashes on batched pairs and puts no separator between the query and
the document.
"""

import numpy as np
import openvino as ov
from transformers import AutoTokenizer

from harness import DEVICE, plugin_properties


class CoreReranker:
    def __init__(self, model_dir, max_length: int = 512, batch: int = 16):
        self.tokenizer = AutoTokenizer.from_pretrained(str(model_dir))
        self.max_length = max_length
        self.batch = batch
        core = ov.Core()
        self.model = core.compile_model(str(model_dir / "openvino_model.xml"), DEVICE, plugin_properties())
        self.inputs = [i.get_any_name() for i in self.model.inputs]
        self.output = self.model.output(0)

    def scores(self, query: str, texts: list[str]) -> np.ndarray:
        out = []
        for i in range(0, len(texts), self.batch):
            enc = self.tokenizer([query] * len(texts[i:i + self.batch]), texts[i:i + self.batch],
                                 truncation="only_second", max_length=self.max_length, padding=True,
                                 return_tensors="np")
            feed = {name: enc[name] for name in self.inputs if name in enc}
            logits = self.model(feed)[self.output]
            out.append(logits[:, 0] if logits.shape[1] == 1 else logits[:, 1] - logits[:, 0])
        return np.concatenate(out)

    def rerank(self, query: str, texts: list[str]) -> list[tuple[int, float]]:
        raw = self.scores(query, texts)
        probs = 1.0 / (1.0 + np.exp(-raw))
        order = np.argsort(-probs)
        return [(int(i), float(probs[i])) for i in order]
