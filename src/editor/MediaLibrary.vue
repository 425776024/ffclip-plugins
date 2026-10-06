<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import type { Project, Asset, FileListing } from '../../packages/client/index.mjs';
import { seconds } from '../../packages/core/project.mjs';
import Icon from './Icon.vue';
import MediaPoster from './MediaPoster.vue';
import LibraryToolbar from './LibraryToolbar.vue';
import LibraryCategoryStrip from './LibraryCategoryStrip.vue';
const props = defineProps<{
  project: Project;
  listing: FileListing | null;
  roots: string[];
  busy: boolean;
  mediaUrl: (id: string) => string;
  filter: string;
}>();
const emit = defineEmits<{
  browse: [path: string];
  import: [path: string];
  append: [asset: Asset];
}>();
const category = ref('all'),
  search = ref(''),
  local = ref(false),
  directory = ref(''),
  folderMenu = ref<HTMLDetailsElement>();
watch(
  () => props.filter,
  () => {
    category.value = 'all';
    local.value = false;
  }
);
watch(
  () => props.listing?.path,
  () => {
    directory.value = '';
  }
);
const categories = computed(() =>
  props.filter === 'media'
    ? [
        { id: 'all', name: '全部素材' },
        { id: 'used', name: '时间轴已用' },
        { id: 'video', name: '视频' },
        { id: 'audio', name: '音频' },
        { id: 'image', name: '图片' }
      ]
    : [
        { id: 'all', name: props.filter === 'audio' ? '全部音频' : '全部图片' },
        { id: 'used', name: '时间轴已用' }
      ]
);
const categoryEntries = computed(() => [
  ...categories.value.map((c) => ({ id: c.id, label: tr(c.name) })),
  { id: 'local', label: tr('本地文件') }
]);
const entries = computed(
  () =>
    props.listing?.entries.filter((entry) =>
      entry.name.toLocaleLowerCase().includes(search.value.trim().toLocaleLowerCase())
    ) ?? []
);
const assets = computed(() =>
  props.project.assets.filter((a) => {
    const kind = category.value;
    return (
      (props.filter === 'media' || props.filter === a.kind) &&
      (kind === 'all' ||
        kind === 'media' ||
        (kind === 'used' &&
          props.project.timeline.tracks.some((t) =>
            t.items.some((i) => i.clip.assetId === a.id)
          )) ||
        kind === a.kind) &&
      a.name.toLocaleLowerCase().includes(search.value.trim().toLocaleLowerCase())
    );
  })
);
function openLocal(root?: string) {
  local.value = true;
  const path = root || props.listing?.path || props.roots[0];
  if (path) emit('browse', path);
  if (folderMenu.value) folderMenu.value.open = false;
}
function chooseCategory(id: string) {
  if (id === 'local') openLocal();
  else {
    local.value = false;
    category.value = id;
  }
}
function closeFolders(event: PointerEvent) {
  if (folderMenu.value && !folderMenu.value.contains(event.target as Node))
    folderMenu.value.open = false;
}
onMounted(() => document.addEventListener('pointerdown', closeFolders));
onBeforeUnmount(() => document.removeEventListener('pointerdown', closeFolders));
function drag(event: DragEvent, asset: Asset) {
  event.dataTransfer?.setData('application/x-videocut-asset', asset.id);
}
function timeLabel(value: number) {
  const s = seconds(value);
  return `${Math.floor(s / 60)
    .toString()
    .padStart(2, '0')}:${Math.floor(s % 60)
    .toString()
    .padStart(2, '0')}`;
}
</script>
<template>
  <aside class="library media-library resource-library panel">
    <div class="library-main">
      <LibraryToolbar
        v-model:query="search"
        :search-label="tr('搜索素材')"
        :action-label="tr('导入')"
        :disabled="busy"
        :count="local ? entries.length : assets.length"
        @action="openLocal()"
      >
        <template #leading>
          <details
            ref="folderMenu"
            class="library-folder-menu"
            @keydown.esc.prevent="folderMenu && (folderMenu.open = false)"
          >
            <summary :title="tr('本地目录')" :aria-label="tr('本地目录')">
              <Icon name="folder" :size="14" />
            </summary>
            <div class="library-folder-popover">
              <button
                :class="{ active: !local }"
                @click="
                  local = false;
                  folderMenu && (folderMenu.open = false);
                "
              >
                {{ tr('作品素材') }}
              </button>
              <small>{{ tr('本地目录') }}</small>
              <button
                v-for="root in roots"
                :key="root"
                data-root
                :title="root"
                :class="{ active: local && listing?.path === root }"
                @click="openLocal(root)"
              >
                <Icon name="folder" :size="14" /><span>{{ root.split('/').pop() || '/' }}</span>
              </button>
            </div>
          </details>
        </template>
      </LibraryToolbar>
      <LibraryCategoryStrip
        :label="tr('素材分类')"
        :entries="categoryEntries"
        :selected="local ? 'local' : category"
        @choose="chooseCategory"
      />
      <template v-if="local"
        ><div class="directory-bar">
          <button
            :disabled="!listing?.parent"
            :title="tr('上一级')"
            @click="listing?.parent && emit('browse', listing.parent)"
          >
            <Icon name="up" :size="14" />
          </button>
          <form @submit.prevent="emit('browse', directory || listing?.path || '')">
            <input
              :value="directory || listing?.path"
              @input="directory = ($event.target as HTMLInputElement).value"
              :aria-label="tr('本地目录路径')"
              :placeholder="tr('输入本地目录')"
            />
          </form>
          <button :title="tr('刷新目录')" @click="emit('browse', directory || listing?.path || '')">
            <Icon name="reset" :size="14" />
          </button>
        </div>
        <div class="library-content">
          <div class="file-list">
            <div v-for="entry in entries" :key="entry.path" class="file-row" :title="entry.path">
              <button
                class="file-entry"
                @click="entry.directory && emit('browse', entry.path)"
                @dblclick="!entry.directory && emit('import', entry.path)"
              >
                <Icon :name="entry.directory ? 'folder' : 'video'" :size="18" /><span>{{
                  entry.name
                }}</span></button
              ><button
                v-if="!entry.directory"
                :aria-label="tr('添加素材')"
                :disabled="busy"
                @click="emit('import', entry.path)"
              >
                <Icon name="plus" :size="15" /></button
              ><Icon v-else name="right" :size="12" />
            </div>
            <div v-if="listing && !listing.entries.length" class="library-empty">
              <Icon name="folder" :size="36" />
              <p>{{ tr('此目录没有素材') }}</p>
              <small>{{ tr('选择其他目录或输入路径') }}</small>
            </div>
            <div v-else-if="listing && !entries.length" class="library-no-results">
              {{ tr('没有匹配的资源') }}
            </div>
          </div>
        </div>
        <div class="library-footnote">{{ tr('直接引用原文件，无需上传') }}</div></template
      >
      <template v-else
        ><div class="library-content">
          <div v-if="assets.length" class="asset-grid">
            <button
              v-for="asset in assets"
              :key="asset.id"
              class="asset-card"
              draggable="true"
              @dragstart="drag($event, asset)"
              @dblclick="emit('append', asset)"
              :title="tr(`${asset.name} · 双击添加到时间轴`)"
            >
              <div class="asset-preview" :class="asset.kind">
                <MediaPoster
                  v-if="asset.kind !== 'audio'"
                  :url="mediaUrl(asset.id)"
                  :kind="asset.kind"
                  :label="asset.name"
                /><Icon v-else name="music" :size="34" /><span class="asset-kind">{{
                  asset.kind === 'video' ? 'SDR' : asset.kind === 'image' ? 'IMG' : ''
                }}</span
                ><span class="asset-duration">{{ timeLabel(asset.duration) }}</span>
              </div>
              <span class="asset-name">{{ asset.name }}</span>
            </button>
          </div>
          <div v-else-if="search || project.assets.length" class="library-no-results">
            {{ tr('没有匹配的资源') }}
          </div>
          <div v-else class="library-empty">
            <div class="empty-media-icon"><Icon name="media" :size="32" /></div>
            <p>{{ tr('导入素材，开始创作') }}</p>
            <small>{{ tr('视频、图片、音频') }}</small
            ><button class="subtle-button" @click="openLocal()">
              <Icon name="plus" :size="14" />{{ tr('浏览本地文件') }}
            </button>
          </div>
        </div>
        <div class="library-footnote">{{ tr('双击添加 · 拖动到时间轴') }}</div></template
      >
    </div>
  </aside>
</template>
