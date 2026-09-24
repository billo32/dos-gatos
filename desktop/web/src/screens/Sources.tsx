import { api } from "../api";
import { useStore } from "../store";
import { Button, GearButton, PageHeader, cx } from "../components/ui";
import { hostOf, sourceText } from "./Playlist";

export const every = (n: number) => (n >= 3600 && n % 3600 === 0 ? `${n / 3600} h` : n >= 60 && n % 60 === 0 ? `${n / 60} min` : `${n} s`);

export function Sources() {
  const { sources, status, edit } = useStore();
  return (
    <>
      <PageHeader title="Sources" sub="REST endpoints the clock polls">
        <Button kind="primary" onClick={() => edit("new")}>+ Add source</Button>
      </PageHeader>
      <div className="flex min-h-0 shrink flex-col overflow-y-auto rounded-xl border border-line">
        {sources.map((s, i) => {
          const v = status?.values[s.name];
          const color = s.color || "#FFFFFF";
          const failed = !!v && v.value == null;
          const pending = !v;
          return (
            <div key={s.name + i} className="box-border flex h-[60px] shrink-0 items-center gap-3.5 border-b border-line-soft bg-white pr-3 pl-4">
              <div className="size-2.5 shrink-0 rounded-[2px]" style={{ background: color, boxShadow: "0 0 0 1px rgba(28,26,23,0.15)" }} />
              <div className="flex min-w-0 grow flex-col gap-0.5">
                <div className={cx("text-[14px] font-semibold", s.off && "text-subtle")}>{s.name}{s.off && <span className="ml-2 text-[12px] font-normal">off in playlist</span>}</div>
                <div className="truncate font-mono text-[11px] text-subtle">{hostOf(s.url)} · every {every(s.every ?? 300)}</div>
              </div>
              <div
                title={failed ? v!.error ?? `HTTP ${v!.status}` : pending ? "No value yet" : `Updated ${v!.at}`}
                className={cx("max-w-[180px] truncate rounded-md px-2 py-1 font-mono text-[13px] font-medium", failed || pending ? "bg-sand-2 text-subtle" : "bg-ink")}
                style={failed || pending ? undefined : { color }}
              >
                {failed ? v!.error ?? "error" : pending ? "--" : sourceText(s, v!.value)}
              </div>
              <GearButton name={s.name} onClick={() => edit(i)} />
            </div>
          );
        })}
        <button type="button" onClick={() => edit("new")} className="h-[52px] shrink-0 border-0 bg-milk text-[14px] font-medium text-ink hover:bg-sand-2">+ Add source</button>
      </div>
      <div className="flex items-baseline justify-between gap-4 text-[12px] leading-[1.45] text-subtle">
        <span>Each source is a REST endpoint. Its value shows up in the playlist as its own screen.</span>
        <Button kind="link" className="!h-auto shrink-0 text-[12px]" onClick={() => api.revealApps()}>Show apps.json in Finder</Button>
      </div>
    </>
  );
}
