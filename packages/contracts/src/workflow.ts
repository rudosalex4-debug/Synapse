/** Public workflow DTOs. Runtime authorization and validation belong to the server. */
export type LearningGoal = 'understand' | 'practice' | 'troubleshoot' | 'learning_path';
export type WorkflowExperience = 'self_study' | 'practice' | 'teaching' | 'participation';
export type RequestStatus = 'draft' | 'open' | 'in_progress' | 'resolved' | 'cancelled' | 'expired' | 'closed_unresolved';
export interface CreateRequest {
  title: string; body: string; learningGoal: LearningGoal; attempt?: string;
  topicId: string; facets: Record<string, string[]>; requiredFacets: string[];
  desiredExperience?: WorkflowExperience; taxonomyVersion: string;
}
/** Reasons refer to one declared competency; this is not a qualification check. */
export interface MatchExplanation {
  topicRelation: 'exact' | 'narrower';
  matchedFacets: string[]; missingOptionalFacets: string[];
  experience: 'matched' | 'different' | 'not_requested'; basis: 'self_declared';
}
/** A time-specific advisory result, without disclosing identities or headcounts. */
export interface RequestReach {
  requestId: string; requestRevision: number;
  status: 'available' | 'no_match' | 'limited'; sampleLimited: boolean; checkedAt: string;
  suggestions: ('add_attempt' | 'review_required_facets' | 'choose_parent_topic')[];
  parentTopicId: string | null;
}
export interface Offer {
  id: string; requestId: string; helperId: string; helperName: string; message: string;
  status: 'pending' | 'accepted' | 'declined' | 'withdrawn'; createdAt: string;
  competency: { id: string; topicId: string; description: string; experienceKind: WorkflowExperience; evidenceStatus: string; provenance: string };
  score: number; narrower: boolean; explanation?: MatchExplanation | null;
}
export interface HelpRequest extends Omit<CreateRequest, 'attempt' | 'desiredExperience'> {
  id: string; authorId: string; authorName: string; attempt: string | null;
  desiredExperience: WorkflowExperience | null; status: RequestStatus; revision: number;
  createdAt: string; updatedAt: string;
  match?: { score: number; competencyId: string; topicId: string; narrower: boolean; explanation?: MatchExplanation };
  myOffer?: Offer | null; conversationId?: string | null;
}
export type Outcome = 'helpful' | 'partly_helpful' | 'not_helpful' | 'no_result';
export interface Conversation {
  id: string; requestId: string; title: string; authorId: string; helperId: string;
  otherUserId: string; otherDisplayName: string; status: 'active' | 'closed';
  moderationClosed?: boolean;
  createdAt: string; closedAt: string | null; outcome: Outcome | null; comment: string | null; nextStep?: string | null;
}
export interface Message {
  id: string; conversationId: string; senderId: string; senderName: string;
  sequence: number; text: string; createdAt: string; clientMessageId: string;
}
export interface RequestPage { items: HelpRequest[]; nextCursor: string | null }
export interface MessagePage { items: Message[]; nextAfter: number }
export type ReportCategory = 'abuse' | 'spam' | 'other';
