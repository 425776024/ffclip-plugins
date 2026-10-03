import { computed, onBeforeUnmount, onMounted, ref, type Ref } from 'vue';

type Divider = 'library' | 'inspector' | 'timeline';
type Layout = {
  library: number;
  preview: number;
  inspector: number;
  workspace: number;
  menu: number;
};
const defaults: Layout = {
  library: 0.23,
  preview: 0.52,
  inspector: 0.25,
  workspace: 0.56,
  menu: 0.4
};
const storageKey = 'videocut.editor.panel-layout.v1';

export function usePanelLayout(
  body: Ref<HTMLElement | undefined>,
  workspace: Ref<HTMLElement | undefined>
) {
  const layout = ref({ ...defaults });
  try {
    const saved = JSON.parse(localStorage.getItem(storageKey) || 'null');
    if (
      saved &&
      ['library', 'preview', 'inspector', 'workspace'].every(
        (key) => Number.isFinite(saved[key]) && saved[key] > 0 && saved[key] < 1
      ) &&
      Math.abs(saved.library + saved.preview + saved.inspector - 1) < 0.001
    )
      layout.value = {
        ...saved,
        menu:
          Number.isFinite(saved.menu) && saved.menu > 0 && saved.menu < 1
            ? saved.menu
            : defaults.menu
      };
  } catch {
    /* Private browsing or invalid storage keeps the default layout. */
  }
  const resizing = ref(false);
  const stacked = ref(window.matchMedia('(max-width: 680px)').matches);
  const values = ref({ library: 23, inspector: 75, timeline: 56 });
  const style = computed(() => ({
    '--library-share': `${layout.value.library}fr`,
    '--preview-share': `${layout.value.preview}fr`,
    '--inspector-share': `${layout.value.inspector}fr`,
    '--workspace-share': `${layout.value.workspace}fr`,
    '--timeline-share': `${1 - layout.value.workspace}fr`,
    '--menu-share': `${layout.value.menu}fr`,
    '--menu-preview-share': `${1 - layout.value.menu}fr`
  }));
  const measure = () => {
    if (!body.value || !workspace.value) return;
    const library = workspace.value.querySelector<HTMLElement>('.library');
    const inspector = workspace.value.querySelector<HTMLElement>('.inspector');
    stacked.value = window.matchMedia('(max-width: 680px)').matches;
    const width = workspace.value.clientWidth;
    const height = workspace.value.clientHeight;
    values.value = {
      library: stacked.value
        ? ((library?.offsetHeight || 0) / height) * 100
        : width
          ? ((library?.offsetWidth || 0) / width) * 100
          : 0,
      inspector: stacked.value
        ? ((inspector?.offsetHeight || 0) / height) * 100
        : width
          ? (1 - (inspector?.offsetWidth || 0) / width) * 100
          : 100,
      timeline: (workspace.value.offsetHeight / body.value.clientHeight) * 100
    };
  };
  let observer: ResizeObserver | undefined;
  onMounted(() => {
    observer = new ResizeObserver(measure);
    if (body.value) observer.observe(body.value);
    if (workspace.value) {
      observer.observe(workspace.value);
      for (const panel of workspace.value.querySelectorAll(':scope > .panel'))
        observer.observe(panel);
    }
    measure();
  });
  onBeforeUnmount(() => observer?.disconnect());
  let gesture:
    | {
        divider: Divider;
        compact: boolean;
        stacked: boolean;
        before: Layout;
        library: number;
        preview: number;
        inspector: number;
        workspace: number;
        timeline: number;
        minima: {
          library: number;
          preview: number;
          inspector: number;
          workspace: number;
          timeline: number;
        };
      }
    | undefined;
  const persist = () => {
    try {
      localStorage.setItem(storageKey, JSON.stringify(layout.value));
    } catch {
      /* Optional preference. */
    }
  };
  function start(divider: Divider) {
    if (!body.value || !workspace.value) return;
    const css = getComputedStyle(body.value);
    const minimum = (name: string) => parseFloat(css.getPropertyValue(`--${name}-min`));
    const dimension = stacked.value && divider !== 'timeline' ? 'height' : 'width';
    const size = (selector: string) =>
      workspace.value!.querySelector<HTMLElement>(selector)?.getBoundingClientRect()[dimension] ||
      0;
    gesture = {
      divider,
      compact: window.matchMedia('(max-width: 800px)').matches,
      stacked: stacked.value,
      before: { ...layout.value },
      library: size('.library'),
      preview: size('.preview-panel'),
      inspector: size('.inspector'),
      workspace: workspace.value.getBoundingClientRect().height,
      timeline:
        body.value.querySelector<HTMLElement>('.timeline-panel')?.getBoundingClientRect().height ||
        0,
      minima: {
        library: minimum('library'),
        preview: minimum('preview'),
        inspector: minimum('inspector'),
        workspace: minimum('workspace'),
        timeline: minimum('timeline')
      }
    };
    resizing.value = true;
  }
  function resize(delta: number) {
    if (!gesture) return;
    const g = gesture;
    const clamp = (value: number, min: number, max: number) => Math.max(min, Math.min(max, value));
    if (g.divider === 'timeline') {
      const total = g.workspace + g.timeline;
      layout.value.workspace =
        clamp(g.workspace + delta, g.minima.workspace, total - g.minima.timeline) / total;
    } else if (g.compact) {
      const menu = g[g.divider];
      const total = menu + g.preview;
      const direction = g.stacked || g.divider === 'library' ? 1 : -1;
      layout.value.menu =
        clamp(
          menu + direction * delta,
          g.stacked ? 100 : g.minima[g.divider],
          total - (g.stacked ? 120 : g.minima.preview)
        ) / total;
    } else {
      let { library, preview, inspector } = g;
      if (g.divider === 'library') {
        library = clamp(
          g.library + delta,
          g.minima.library,
          g.library + g.preview - g.minima.preview
        );
        preview = g.preview + g.library - library;
      } else {
        inspector = clamp(
          g.inspector - delta,
          g.minima.inspector,
          g.inspector + g.preview - g.minima.preview
        );
        preview = g.preview + g.inspector - inspector;
      }
      // Fractions preserve the layout across window sizes; only neighboring panels move.
      const total = library + preview + inspector;
      layout.value = {
        ...layout.value,
        library: library / total,
        preview: preview / total,
        inspector: inspector / total
      };
    }
  }
  function finish(cancelled: boolean) {
    if (!gesture) return;
    if (cancelled) layout.value = gesture.before;
    else persist();
    gesture = undefined;
    resizing.value = false;
  }
  function step(divider: Divider, delta: number) {
    start(divider);
    resize(delta);
    finish(false);
  }
  function reset(divider: Divider) {
    finish(true);
    if (divider === 'timeline') layout.value.workspace = defaults.workspace;
    else if (window.matchMedia('(max-width: 800px)').matches) layout.value.menu = defaults.menu;
    else layout.value = { ...defaults, workspace: layout.value.workspace, menu: layout.value.menu };
    persist();
  }
  return { style, resizing, stacked, values, start, resize, finish, step, reset };
}
