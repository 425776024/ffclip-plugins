import { createApp } from 'vue';
import Editor from './Editor.vue';
import './editor.css';
import { followSystemLanguage } from './i18n';

const stopFollowingLanguage = followSystemLanguage();
const app = createApp(Editor);
app.onUnmount(stopFollowingLanguage);
app.mount('#app');
