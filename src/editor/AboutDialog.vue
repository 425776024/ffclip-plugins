<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from 'vue';
import Icon from './Icon.vue';
import { useI18n } from './i18n';
import wechatQr from './assets/contact/wechat-qr.jpg';
import officialQr from './assets/contact/wechat-official-qr.jpg';

const emit = defineEmits<{ close: [] }>();
const { tr } = useI18n();
const dialog = ref<HTMLDialogElement>();
const closeButton = ref<HTMLButtonElement>();

onMounted(() => {
  dialog.value?.showModal();
  closeButton.value?.focus();
});
onBeforeUnmount(() => dialog.value?.close());

function backdropClick(event: MouseEvent) {
  if (event.target !== dialog.value || !dialog.value) return;
  const bounds = dialog.value.getBoundingClientRect();
  if (
    event.clientX < bounds.left ||
    event.clientX > bounds.right ||
    event.clientY < bounds.top ||
    event.clientY > bounds.bottom
  )
    emit('close');
}
</script>

<template>
  <dialog
    ref="dialog"
    class="help-dialog panel about-dialog"
    aria-labelledby="about-dialog-title"
    @cancel.prevent="emit('close')"
    @click="backdropClick"
    @keydown.stop
    @paste.stop
  >
    <div class="panel-heading">
      <strong id="about-dialog-title">{{ tr('关于 ffclip') }}</strong>
      <button ref="closeButton" :aria-label="tr('关闭关于')" @click="emit('close')">
        <Icon name="close" />
      </button>
    </div>
    <div class="about-body">
      <div class="about-contacts">
        <figure class="about-contact">
          <a
            class="about-qr-image"
            :href="wechatQr"
            target="_blank"
            rel="noopener noreferrer"
            :aria-label="tr('查看微信好友二维码原图')"
            :title="tr('点击查看原图')"
          >
            <img
              :src="wechatQr"
              width="888"
              height="867"
              :alt="tr('微信好友二维码，扫码添加作者')"
            />
          </a>
          <figcaption>
            <strong>{{ tr('微信好友') }}</strong>
            <span>{{ tr('扫一扫，加我微信') }}</span>
          </figcaption>
        </figure>
        <figure class="about-contact">
          <a
            class="about-qr-image"
            :href="officialQr"
            target="_blank"
            rel="noopener noreferrer"
            :aria-label="tr('查看微信公众号二维码原图')"
            :title="tr('点击查看原图')"
          >
            <img
              :src="officialQr"
              width="1050"
              height="1164"
              :alt="tr('新身数字人微信公众号二维码')"
            />
          </a>
          <figcaption>
            <strong>{{ tr('新身数字人') }}</strong>
            <span>{{ tr('微信扫码，关注公众号') }}</span>
          </figcaption>
        </figure>
      </div>
      <a
        class="about-link"
        href="https://ffclip.com"
        target="_blank"
        rel="noopener noreferrer"
        :aria-label="tr('ffclip 官网（在新标签页打开）')"
      >
        <Icon name="link" :size="22" />
        <span class="about-link-label">
          <strong>{{ tr('官网') }}</strong>
          <span>ffclip.com</span>
        </span>
        <span class="about-link-action">{{ tr('访问官网') }}<Icon name="right" /></span>
      </a>
      <a
        class="about-link"
        href="https://space.bilibili.com/98643795"
        target="_blank"
        rel="noopener noreferrer"
        :aria-label="tr('B 站主页（在新标签页打开）')"
      >
        <Icon name="video" :size="22" />
        <span class="about-link-label">
          <strong>{{ tr('B 站主页') }}</strong>
          <span>bilibili · 98643795</span>
        </span>
        <span class="about-link-action">{{ tr('打开主页') }}<Icon name="right" /></span>
      </a>
    </div>
  </dialog>
</template>
