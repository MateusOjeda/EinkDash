import {
	addDoc,
	collection,
	deleteDoc,
	doc,
	getDocs,
	limit,
	onSnapshot,
	query,
	serverTimestamp,
	setDoc,
	updateDoc,
	where,
	writeBatch,
} from "firebase/firestore";
import { db } from "@/firebase/config";

export type HabitIcon = {
	width: number;
	height: number;
	format: "gray2bpp";
	data: string;
};

export type Habit = {
	id: string;
	name: string;
	position: number;
	// Reservado para o ícone personalizado (2 bpp/Base64).
	icon?: HabitIcon | null;
};

export function localDateKey(date: Date): string {
	const year = date.getFullYear();
	const month = String(date.getMonth() + 1).padStart(2, "0");
	const day = String(date.getDate()).padStart(2, "0");
	return `${year}-${month}-${day}`;
}

export function lastSevenDays(): Date[] {
	const now = new Date();
	return Array.from(
		{ length: 7 },
		(_, index) =>
			new Date(
				now.getFullYear(),
				now.getMonth(),
				now.getDate() - 6 + index,
			),
	);
}

export function listenHabits(
	callback: (habits: Habit[]) => void,
	onError: (error: Error) => void,
) {
	return onSnapshot(
		collection(db, "habits"),
		(snapshot) => {
			const habits = snapshot.docs.map((snapshotDoc) => {
				const data = snapshotDoc.data();
				return {
					id: snapshotDoc.id,
					name: String(data.name ?? ""),
					position:
						typeof data.position === "number" ? data.position : 0,
					icon: data.icon ?? null,
				} as Habit;
			});
			habits.sort(
				(a, b) =>
					a.position - b.position || a.name.localeCompare(b.name),
			);
			callback(habits);
		},
		onError,
	);
}

export function listenCompletedDays(
	habitId: string,
	startDay: string,
	endDay: string,
	callback: (days: Set<string>) => void,
	onError: (error: Error) => void,
) {
	const q = query(
		collection(db, "habits", habitId, "completions"),
		where("date", ">=", startDay),
		where("date", "<=", endDay),
	);
	return onSnapshot(
		q,
		(snapshot) => callback(new Set(snapshot.docs.map((item) => item.id))),
		onError,
	);
}

export async function createHabit(name: string, habits: Habit[]) {
	const nextPosition = habits.length
		? Math.max(...habits.map((habit) => habit.position)) + 1
		: 0;
	await addDoc(collection(db, "habits"), {
		name: name.trim(),
		position: nextPosition,
		icon: null,
		createdAt: serverTimestamp(),
		updatedAt: serverTimestamp(),
	});
}

export async function toggleCompletedDay(
	habitId: string,
	day: string,
	completed: boolean,
) {
	const reference = doc(db, "habits", habitId, "completions", day);
	if (completed) {
		await deleteDoc(reference);
	} else {
		await setDoc(reference, { date: day, completedAt: serverTimestamp() });
	}
}

export async function swapHabits(first: Habit, second: Habit) {
	const batch = writeBatch(db);
	batch.update(doc(db, "habits", first.id), {
		position: second.position,
		updatedAt: serverTimestamp(),
	});
	batch.update(doc(db, "habits", second.id), {
		position: first.position,
		updatedAt: serverTimestamp(),
	});
	await batch.commit();
}

export async function renameHabit(id: string, name: string) {
	await updateDoc(doc(db, "habits", id), {
		name: name.trim(),
		updatedAt: serverTimestamp(),
	});
}

export async function deleteHabitWithHistory(id: string) {
	// Ao apagar um documento, o Firestore não apaga automaticamente as subcoleções.
	// Processamos o histórico em lotes para evitar esse problema.
	const history = collection(db, "habits", id, "completions");
	while (true) {
		const snapshot = await getDocs(query(history, limit(200)));
		if (snapshot.empty) break;
		const batch = writeBatch(db);
		snapshot.docs.forEach((item) => batch.delete(item.ref));
		await batch.commit();
	}
	await deleteDoc(doc(db, "habits", id));
}

export async function updateHabitIcon(id: string, icon: HabitIcon | null) {
	await updateDoc(doc(db, "habits", id), {
		icon,
		updatedAt: serverTimestamp(),
	});
}
