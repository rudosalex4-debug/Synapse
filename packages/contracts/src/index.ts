/** Public DTOs. Runtime validation is enforced by the backend. */
export interface BootstrapResponse { mode: 'demo' | 'max'; maxConfigured: boolean; version: string; pilotMode?: boolean }
export interface Topic { id: string; parent_id: string | null; level: number; label: string; aliases: string[]; active: boolean; description?: string }
export interface SessionUser { id: string; displayName: string; provenance: 'demo' | 'self_declared' }
export interface AuthResponse { token: string; expiresAt: string; user: SessionUser }
export interface ApiError { error: { code: string; message: string; requestId?: string } }
export type ExperienceKind = 'self_study' | 'practice' | 'teaching' | 'participation';
export interface CompetencyInput { id?: string; topicId: string; facets: Record<string, string[]>; experienceKind: ExperienceKind; description: string; evidenceUrl?: string; evidenceVisibility: 'private' | 'participants' }
export interface Competency extends CompetencyInput { id: string; evidenceStatus: 'none' | 'unreviewed'; provenance: 'demo' | 'self_declared' }
export interface ProfileInput { displayName: string; bio: string; availableToHelp: boolean; maxActiveConversations: number; competencies: CompetencyInput[] }
export interface Profile extends Omit<ProfileInput, 'competencies'> { id: string; revision: number; provenance: 'demo' | 'self_declared'; competencies: Competency[]; /** Read-only historical skills outside the active MVP tracks. */ archivedCompetencies?: Competency[] }
export * from './workflow.js';
