import { ApiError, api, errorMessage } from './api';
export type { Conversation, CreateRequest, HelpRequest, LearningGoal, Message, MessagePage, Offer, Outcome, ReportCategory, RequestPage, RequestStatus, RequestReach, MatchExplanation, WorkflowExperience } from '../../../packages/contracts/src/workflow';

/** A key survives an uncertain response, but not a changed draft or a completed action. */
export class MutationKeys {
  private entries = new Map<string, { signature: string; id: string }>();
  constructor(private readonly createId: () => string = () => crypto.randomUUID()) {}
  key(action: string, payload: unknown): string {
    const signature = JSON.stringify(payload);
    const previous = this.entries.get(action);
    if (previous?.signature === signature) return previous.id;
    const id = this.createId();
    this.entries.set(action, { signature, id });
    return id;
  }
  clear(action: string): void { this.entries.delete(action); }
}

export function workflowRead<T>(path: string, token: string): Promise<T> {
  return api<T>(path, { token });
}
export function workflowWrite<T>(path: string, token: string, body: unknown, idempotencyKey?: string, revision?: number): Promise<T> {
  return api<T>(path, { token, body, method: revision === undefined ? 'POST' : 'PUT', idempotencyKey, revision });
}
export function workflowError(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.status === 401) return 'Время входа истекло. Войдите снова: набранный текст останется здесь.';
    if (error.status === 403) return 'Действие недоступно для этого участника или общение ограничено.';
    if (error.status === 404) return 'Вопрос или диалог больше недоступен. Обновите список.';
    if (error.status === 409 || error.status === 428) return 'Состояние изменилось: вопрос мог быть занят или закрыт, либо достигнут лимит. Обновите данные и проверьте их перед повтором. Ваш текст сохранён на экране.';
    if (error.status === 400 || error.status === 422) return 'Проверьте длину текста, выбранную тему и уточнения. Для вопроса нужно от 30 до 2000 символов.';
  }
  return errorMessage(error);
}
