<script setup lang="ts">
import { useI18n } from './i18n';
const { tr } = useI18n();
import { computed, ref, watch } from 'vue';
import type { Project, Asset, FileListing } from '../../packages/client/index.mjs';
import { seconds } from '../../packages/core/project.mjs';
import Icon from './Icon.vue';
import MediaPoster from './MediaPoster.vue';
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
  directory = ref('');
watch(
  () => props.filter,
  () => {
    category.value = 'all';
    local.value = false;
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
      a.name.toLowerCase().includes(search.value.toLowerCase())
    );
  })
);
function openLocal(root?: string) {
  local.value = true;
  emit('browse', root || props.listing?.path || props.roots[0]);
}
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
  <aside class="library panel">
    <nav class="library-categories">
      <div class="category-heading"><Icon name="up" :size="12" />{{ tr('素材') }}</div>
      <button
        v-for="c in categories"
        :key="c.id"
        :class="{ active: !local && category === c.id }"
        @click="
          local = false;
          category = c.id;
        "
      >
        {{ tr(c.name) }}
      </button>
      <div class="category-heading local-heading">
        <Icon name="up" :size="12" />{{ tr('本地目录') }}
      </div>
      <button
        v-for="root in roots"
        :key="root"
        :class="{ active: local && listing?.path === root }"
        :title="root"
        @click="openLocal(root)"
      >
        <Icon name="folder" :size="14" /><span>{{ root.split('/').pop() || '/' }}</span>
      </button>
    </nav>
    <div class="library-main">
      <div class="library-toolbar">
        <button class="import-button" @click="openLocal()">
          <Icon name="plus" :size="15" />{{ tr('导入') }}</button
        ><button :title="tr('浏览本地文件')" @click="openLocal()">
          <Icon name="folder" :size="16" />
        </button>
        <div class="search-field">
          <Icon name="search" :size="14" /><input
            v-model="search"
            :aria-label="tr('搜索素材')"
            placeholder=""
          />
        </div>
        <button :class="{ active: !local }" :title="tr('素材网格')" @click="local = false">
          <Icon name="grid" :size="15" />
        </button>
      </div>
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
        <div class="file-list">
          <div
            v-for="entry in listing?.entries.filter((e) =>
              e.name.toLowerCase().includes(search.toLowerCase())
            )"
            :key="entry.path"
            class="file-row"
            :title="entry.path"
          >
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
        </div>
        <div class="library-footnote">{{ tr('直接引用原文件，无需上传') }}</div></template
      >
      <template v-else
        ><div class="asset-grid">
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
        <div v-if="!assets.length" class="library-empty">
          <div class="empty-media-icon"><Icon name="media" :size="32" /></div>
          <p>{{ tr('导入素材，开始创作') }}</p>
          <small>{{ tr('视频、图片、音频') }}</small
          ><button class="subtle-button" @click="openLocal()">
            <Icon name="plus" :size="14" />{{ tr('浏览本地文件') }}
          </button>
        </div>
        <div class="library-footnote">{{ tr('双击添加 · 拖动到时间轴') }}</div></template
      >
    </div>
  </aside>
</template>
