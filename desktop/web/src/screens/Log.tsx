import { useLayoutEffect, useRef, useState } from "react";
import { api, errText } from "../api";
import { useStore } from "../store";
import { Button, PageHeader, Switch, cx } from "../components/ui";

const DOT = { info: "#B7AFA3", warn: "#EDB54A", error: "#E5484D" };

export function Log() {
  const { log, flash } = useStore();
  const [problems, setProblems] = useState(false);
  const box = useRef<HTMLDivElement>(null);
  const atTop = useRef(true);
  const rows = problems ? log.filter((l) => l.lv !== "info") : log;

  // newest first: keep the view still while the user reads older lines
  const prevLen = useRef(rows.length);
  useLayoutEffect(() => {
    const el = box.current;
    if (el && !atTop.current && rows.length > prevLen.current) el.scrollTop += (rows.length - prevLen.current) * 40;
    prevLen.current = rows.length;
  }, [rows.length]);

  const clear = async () => {
    await api.logClear().catch(() => {});
    useStore.setState({ log: [] });
  };
  const exportLog = async () => {
    try {
      const p = await api.logExport();
      if (p) flash("Log exported");
    } catch (e) {
      flash(errText(e), true);
    }
  };

  return (
    <>
      <PageHeader title="Log" sub="What the app and the clock are saying">
        <label className="mr-2 flex items-center gap-2 text-[13px] font-medium">
          <Switch on={problems} onChange={setProblems} label="Problems only" />
          Problems only
        </label>
        <Button onClick={clear}>Clear</Button>
        <Button onClick={exportLog}>Export</Button>
      </PageHeader>
      <div
        ref={box}
        role="log"
        aria-label="Agent log"
        onScroll={(e) => (atTop.current = e.currentTarget.scrollTop < 8)}
        className="min-h-0 grow overflow-auto rounded-xl border border-line font-mono text-[12px]"
      >
        {rows.length === 0 && <div className="px-4 py-3 text-subtle">{problems ? "No problems." : "Nothing yet."}</div>}
        {rows.map((r, i) => (
          <div key={rows.length - i} className={cx("flex h-10 items-center gap-3.5 border-b border-sand-2 px-4", r.lv === "error" ? "bg-[#FFF8F7]" : "bg-white")}>
            <div className="w-16 shrink-0 text-subtle">{r.t}</div>
            <div className="size-2 shrink-0 rounded-full" style={{ background: DOT[r.lv] }} aria-label={r.lv} />
            <div className="truncate text-ink" title={r.m}>{r.m}</div>
          </div>
        ))}
      </div>
    </>
  );
}
