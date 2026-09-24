import { useEffect, useId, useRef, type ButtonHTMLAttributes, type InputHTMLAttributes, type ReactNode, type SelectHTMLAttributes } from "react";

const cx = (...c: (string | false | null | undefined)[]) => c.filter(Boolean).join(" ");

type BtnKind = "primary" | "secondary" | "danger" | "link";
export function Button({ kind = "secondary", small, className, ...p }: ButtonHTMLAttributes<HTMLButtonElement> & { kind?: BtnKind; small?: boolean }) {
  const base = "inline-flex items-center justify-center rounded-[9px] text-[13px] font-medium whitespace-nowrap disabled:opacity-45";
  const k = {
    primary: "h-[38px] px-4 bg-ink text-milk border-0",
    secondary: cx(small ? "h-[34px]" : "h-[38px]", "px-3.5 bg-white text-ink border border-field"),
    danger: "h-[38px] px-3 bg-transparent text-danger border-0",
    link: "h-8 px-0 bg-transparent text-ginger-text border-0",
  }[kind];
  return <button type="button" className={cx(base, k, className)} {...p} />;
}

export function Switch({ on, onChange, label, disabled, title }: { on: boolean; onChange: (v: boolean) => void; label: string; disabled?: boolean; title?: string }) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={on}
      aria-label={label}
      title={title}
      disabled={disabled}
      onClick={() => onChange(!on)}
      className={cx("flex h-[26px] w-11 shrink-0 rounded-[13px] border-0 p-[3px] transition-colors disabled:opacity-45", on ? "justify-end bg-toggle-on" : "justify-start bg-toggle-off")}
    >
      <span className="block size-5 rounded-[10px] bg-white shadow-[0_1px_2px_rgba(0,0,0,0.25)]" />
    </button>
  );
}

export const inputCls = "h-10 w-full box-border rounded-[10px] border border-field bg-white px-3 text-[14px] text-ink outline-none focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-ginger";

export function Input({ mono, invalid, className, ...p }: InputHTMLAttributes<HTMLInputElement> & { mono?: boolean; invalid?: boolean }) {
  return <input className={cx(inputCls, mono && "font-mono text-[13px]", invalid && "border-danger", className)} spellCheck={false} autoCorrect="off" autoCapitalize="off" {...p} />;
}

export function Select({ className, ...p }: SelectHTMLAttributes<HTMLSelectElement>) {
  return <select className={cx(inputCls, "appearance-auto", className)} {...p} />;
}

export function Field({ label, error, hint, children, id }: { label: string; error?: string | null; hint?: ReactNode; children: (id: string) => ReactNode; id?: string }) {
  const auto = useId();
  const fid = id ?? auto;
  return (
    <div className="flex min-w-0 flex-col gap-1.5">
      <label htmlFor={fid} className="text-[13px] font-semibold text-ink">{label}</label>
      {children(fid)}
      {error ? <div className="text-[12px] leading-[1.45] text-danger">{error}</div> : hint ? <div className="text-[12px] leading-[1.45] text-subtle">{hint}</div> : null}
    </div>
  );
}

export const SWATCHES = ["#FFB23F", "#FF7A3D", "#FF4B4B", "#3DDC84", "#4DA3FF", "#B57BFF", "#FFF1DC"];

export function Swatches({ value, onChange }: { value: string; onChange: (c: string) => void }) {
  const v = value.toUpperCase();
  return (
    <div role="radiogroup" aria-label="Color" className="flex h-10 items-center gap-[7px]">
      {SWATCHES.map((c) => (
        <button
          key={c}
          type="button"
          role="radio"
          aria-checked={c === v}
          aria-label={c}
          onClick={() => onChange(c)}
          className="size-6 rounded-md border-0 p-0"
          style={{ background: c, boxShadow: c === v ? "0 0 0 2px #FFFFFF, 0 0 0 4px #1C1A17" : "inset 0 0 0 1px rgba(28,26,23,0.15)" }}
        />
      ))}
    </div>
  );
}

export function GearButton({ name, onClick }: { name: string; onClick: () => void }) {
  return (
    <button type="button" onClick={onClick} aria-label={`${name} settings`} className="flex size-9 shrink-0 items-center justify-center rounded-[9px] border border-transparent bg-transparent text-muted hover:bg-sand-2">
      <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
        <path d="M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z" />
      </svg>
    </button>
  );
}

export function PageHeader({ title, sub, children }: { title: string; sub: ReactNode; children?: ReactNode }) {
  return (
    <div className="flex shrink-0 items-center justify-between gap-4">
      <div className="flex flex-col gap-0.5">
        <h1 className="m-0 font-display text-[22px] font-semibold tracking-[-0.01em]">{title}</h1>
        <div className="text-[13px] text-subtle">{sub}</div>
      </div>
      <div className="flex items-center gap-2">{children}</div>
    </div>
  );
}

export function Eyebrow({ children, as: As = "div", className }: { children: ReactNode; as?: "div" | "h2"; className?: string }) {
  return <As className={cx("m-0 font-mono text-[11px] font-normal uppercase tracking-[0.08em] text-subtle", className)}>{children}</As>;
}

/** Modal dialog: Esc/Cancel closes, Enter submits (outside textareas/buttons), focus is trapped. */
export function Modal({ title, width = 480, onClose, onSubmit, footer, children }: { title: string; width?: number; onClose: () => void; onSubmit?: () => void; footer: ReactNode; children: ReactNode }) {
  const ref = useRef<HTMLDivElement>(null);
  const titleId = useId();
  const closeRef = useRef(onClose);
  closeRef.current = onClose;
  useEffect(() => {
    const prev = document.activeElement as HTMLElement | null;
    const el = ref.current!;
    const first = el.querySelector<HTMLElement>("input, select, textarea") ?? el.querySelector<HTMLElement>("button");
    first?.focus();
    // Esc works even when focus fell out of the dialog (e.g. a button got disabled)
    const esc = (e: KeyboardEvent) => {
      if (e.key === "Escape" && !el.contains(document.activeElement)) closeRef.current();
    };
    document.addEventListener("keydown", esc);
    return () => {
      document.removeEventListener("keydown", esc);
      prev?.focus?.();
    };
  }, []);
  const onKey = (e: React.KeyboardEvent) => {
    if (e.key === "Escape") {
      e.stopPropagation();
      onClose();
    } else if (e.key === "Enter" && onSubmit) {
      const t = e.target as HTMLElement;
      if (t.tagName === "INPUT" || t.tagName === "SELECT") {
        e.preventDefault();
        onSubmit();
      }
    } else if (e.key === "Tab") {
      const items = [...ref.current!.querySelectorAll<HTMLElement>('button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea, [tabindex]:not([tabindex="-1"])')];
      if (!items.length) return;
      const i = items.indexOf(document.activeElement as HTMLElement);
      if (e.shiftKey && i <= 0) {
        e.preventDefault();
        items[items.length - 1].focus();
      } else if (!e.shiftKey && i === items.length - 1) {
        e.preventDefault();
        items[0].focus();
      }
    }
  };
  return (
    <div className="absolute inset-0 z-10 flex items-center justify-center bg-[rgba(28,26,23,0.32)]" onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div
        ref={ref}
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        onKeyDown={onKey}
        className="box-border flex max-h-[calc(100%-40px)] flex-col overflow-hidden rounded-2xl bg-white shadow-[0_24px_60px_rgba(28,26,23,0.28)]"
        style={{ width }}
      >
        <div className="box-border flex h-[60px] shrink-0 items-center justify-between border-b border-divider pr-3 pl-[22px]">
          <h2 id={titleId} className="m-0 text-[17px] font-semibold">{title}</h2>
          <button type="button" onClick={onClose} aria-label="Close" className="flex size-9 items-center justify-center rounded-[9px] border-0 bg-transparent text-muted hover:bg-sand-2">
            <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18" /></svg>
          </button>
        </div>
        <div className="box-border flex min-h-0 grow flex-col gap-4 overflow-auto px-[22px] py-5">{children}</div>
        <div className="box-border flex h-16 shrink-0 items-center justify-between gap-2 border-t border-divider bg-milk pr-4 pl-3">{footer}</div>
      </div>
    </div>
  );
}

export function SettingsGroup({ title, children }: { title: string; children: ReactNode }) {
  return (
    <section className="flex flex-col gap-2">
      <Eyebrow as="h2" className="pl-1">{title}</Eyebrow>
      <div className="overflow-hidden rounded-xl border border-line bg-white [&>*:last-child]:border-b-0">{children}</div>
    </section>
  );
}

export function SettingsRow({ title, hint, htmlFor, children, disabled }: { title: ReactNode; hint?: ReactNode; htmlFor?: string; children?: ReactNode; disabled?: boolean }) {
  const T = htmlFor ? "label" : "div";
  return (
    <div className={cx("box-border flex min-h-[52px] items-center justify-between gap-3 border-b border-row px-3.5 py-2", disabled && "opacity-55")}>
      <div className="flex min-w-0 flex-col gap-0.5">
        <T htmlFor={htmlFor} className="text-[14px] font-medium">{title}</T>
        {hint && <div className="text-[12px] leading-[1.45] text-subtle">{hint}</div>}
      </div>
      {children}
    </div>
  );
}

export { cx };
