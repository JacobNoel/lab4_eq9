/******************************************************************************
* H2023
* LABORATOIRE 4, Systèmes embarqués et temps réel
* Ébauche de code pour le pilote utilisant les interruptions
* Marc-André Gardner, mars 2019
*
* Ce fichier contient la structure du pilote qu'il vous faut implémenter. Ce
* pilote fonctionne avec des interruptions, c'est-à-dire qu'il vérifie la valeur
* des touches appuyées que lorsqu'une touche est effectivement enfoncée.
*
* Prenez le temps de lire attentivement les notes de cours et les commentaires
* contenus dans ce fichier, ils contiennent des informations cruciales.
*
* Inspiré de http://derekmolloy.ie/writing-a-linux-kernel-module-part-1-introduction/
*/

// Inclusion des en-têtes nécessaires
// Vous pouvez en ajouter, mais n'oubliez pas que vous n'avez PAS
// accès la libc! Vous ne pouvez vous servir que des fonctions fournies
// par le noyau Linux.
#include <linux/init.h>             // Macros spécifiques des fonctions d'un module
#include <linux/module.h>           // En-tête général des modules noyau
#include <linux/device.h>           // Pour créer un pilote
#include <linux/kernel.h>           // Différentes définitions de types liés au noyau
#include <linux/gpio.h>             // Pour accéder aux GPIO du Raspberry Pi
#include <linux/fs.h>               // Pour accéder au système de fichier et créer un fichier spécial dans /dev
#include <linux/uaccess.h>          // Permet d'accéder à copy_to_user et copy_from_user
#include <linux/delay.h>            // Fonctions d'attente, en particulier msleep
#include <linux/string.h>           // Différentes fonctions de manipulation de string, plus memset et memcpy
#include <linux/mutex.h>            // Mutex et synchronisation
#include <linux/interrupt.h>        // Définit les symboles pour les interruptions et les tasklets
#include <linux/atomic.h>           // Synchronisation par valeur atomique

// Le nom de notre périphérique et le nom de sa classe
#define DEV_NAME "claviersetr"
#define CLS_NAME "setr"

// Le nombre de caractères pouvant être contenus dans le buffer circulaire
#define TAILLE_BUFFER 256

#define NB_LIGNES 4
#define NB_COLONNES 3
#define NB_GPIOS NB_LIGNES + NB_COLONNES  
#define NB_PATTERNS 4


// On déclare tout de suite le nom de la fonction gérant les interruptions
static irq_handler_t  setr_irq_handler(unsigned int irq, void *dev_id, struct pt_regs *regs);

// Déclaration des fonctions pour gérer notre fichier
// Nous ne définissons que open(), close() et read()
static int     dev_open(struct inode *, struct file *);
static int     dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);

static struct file_operations fops =
{
   .open = dev_open,
   .read = dev_read,
   .release = dev_release,
};

// Variables globales et statiques utilisées dans le driver
static int    majorNumber;                  // Numéro donné par le noyau à notre pilote
static char   data[TAILLE_BUFFER] = {0};    // Buffer circulaire contenant les caractères du clavier
static size_t posCouranteLecture = 0;       // Position de la prochaine lecture dans le buffer
static size_t posCouranteEcriture = 0;      // Position de la prochaine écriture dans le buffer

static struct class*  setrClasse  = NULL;   // Contiendra les informations sur la classe de notre pilote
static struct device* setrDevice = NULL;    // Contiendra les informations sur le périphérique associé

static struct mutex sync;                   // Mutex servant à synchroniser les accès au buffer
static atomic_t irqActif = ATOMIC_INIT(1);  // Pour déterminer si les interruptions doivent être traitées

// 4 GPIO doivent être assignés pour l'écriture, et 3 en lecture (voir énoncé)
// Nous vous proposons les choix suivants, mais ce n'est pas obligatoire
static int  gpiosEcrire[] = {5, 6, 13, 19};             // Correspond aux pins 29, 31, 33 et 35
static int  gpiosLire[] = {12, 16, 20};             // Correspond aux pins 32, 36 et 38
// Les noms des différents GPIO
static char* gpiosEcrireNoms[] = {"OUT1", "OUT2", "OUT3", "OUT4"};
static char* gpiosLireNoms[] = {"IN1", "IN2", "IN3"};

static unsigned int irqId[3];               // Contient les numéros d'interruption pour chaque broche de lecture

// Les patrons de balayage (une seule ligne doit être active à la fois)
static int   patterns[4][4] = {
    {1, 0, 0, 0},
    {0, 1, 0, 0},
    {0, 0, 1, 0},
    {0, 0, 0, 1}
};

// Les valeurs du clavier, selon la ligne et la colonne actives
static char valeursClavier[4][3] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'}
};

// Permet de se souvenir du dernier état du clavier,
// pour ne pas répéter une touche qui était déjà enfoncée.
static int dernierEtat[4][3] = {0};

// Durée (en ms) du "debounce" des touches
//static int dureeDebounce = 50;


static struct tasklet_struct tasklet;


void func_tasklet_polling(unsigned long paramf){
    // Cette fonction est le coeur d'exécution du tasklet
    // Elle fait à peu de choses près la même chose que le kthread
    // dans le pilote que vous avez précédemment écrit (par polling),
    // à savoir qu'elle balaye les différentes lignes pour trouver quelle
    // touche est pressée.
    // Une différence majeure est que ce tasklet ne contient pas de boucle,
    // il ne s'exécute qu'une seule fois par interruption!
    int patternIdx, ligneIdx, colIdx, val;

    // TODO
    // Écrivez le code permettant
    // 1) D'éviter le traitement de nouvelles interruptions : nous allons changer
    //      les niveaux des broches de lecture, il ne faut pas que ce soit interprété
    //      comme une nouvelle pression sur une touche, sinon ce tasklet sera rappelé
    //      en boucle! Vous êtes libres d'utiliser l'approche que vous souhaitez pour
    //      éviter cela, mais la variable atomique irqActif pourrait vous être utile...
    // 2) De passer au travers de tous les patrons de balayage
    // 3) Pour chaque patron, vérifier la valeur des lignes d'entrée
    // 4) Selon ces valeurs et le contenu de dernierEtat, déterminer si une nouvelle touche a été pressée
    // 5) Mettre à jour le buffer et dernierEtat en vous assurant d'éviter les race conditions avec le reste du module
    // 6) Remettre toutes les lignes à 1 (pour réarmer l'interruption)
    // 7) Réactiver le traitement des interruptions

    //struct clavier_dev *dev = (struct clavier_dev *) paramf;
    
    // 1) Désactive les interruptions pour éviter le traitement de nouvelles interruptions
    atomic_set(&irqActif, 0);

/*
    // 2) Parcours de tous les patrons de balayage
    for (patternIdx = 0; patternIdx < NB_PATTERNS; patternIdx++) {
        struct pattern *p = &patterns[patternIdx];
        int press = 0;

        // 3) Vérifie la valeur des lignes d'entrée
        for (ligneIdx = 0; ligneIdx < NB_LIGNES; ligneIdx++) {
            val = gpio_get_value(dev->lignes[ligneIdx]);
            p->lignes[ligneIdx] = val;
        }

        // 4) Détermine si une nouvelle touche a été pressée
        for (colIdx = 0; colIdx < NB_COLONNES; colIdx++) {
            int allPressed = 1;
            for (ligneIdx = 0; ligneIdx < NB_LIGNES; ligneIdx++) {
                if (p->lignes[ligneIdx] != etatPrecedent[ligneIdx][colIdx]) {
                    allPressed = 0;
                    break;
                }
            }
            if (allPressed && !etatPrecedent[NB_LIGNES][colIdx]) {
                press = 1;
                break;
            }
        }

        // 5) Met à jour le buffer et dernierEtat
        if (press) {
            int pos = (posCouranteEcriture + 1) % TAILLE_BUFFER;
            if (pos != posCouranteLecture) {
                buffer[pos] = mappingTouches[p->caracteres[colIdx]];
                posCouranteEcriture = pos;
            }
        }
        memcpy(etatPrecedent, p->lignes, sizeof(int) * (NB_LIGNES + 1));

        // 6) Remet toutes les lignes à 1 (pour réarmer l'interruption)
        for (ligneIdx = 0; ligneIdx < NB_LIGNES; ligneIdx++) {
            gpio_set_value(dev->lignes[ligneIdx], 1);
        }*/
        //Boucle sur chaque ligne de pattern qui correspond à un patron de balayage
        // 2) Parcours de tous les patrons de balayage
        for (patternIdx=0;patternIdx<4;patternIdx++){
            //Assigne la valeur de pattern au GPIO (1 ou 0)
            //printk(KERN_INFO "applique le pattern: %d\n",patternIdx);

            for (ligneIdx=0;ligneIdx<4;ligneIdx++){
                //printk(KERN_INFO "valeur %d a la ligne %d\n",patterns[patternIdx][ligneIdx], ligneIdx);
                gpio_set_value(gpiosEcrire[ligneIdx], patterns[patternIdx][ligneIdx]);
            }

            for (colIdx=0;colIdx<3;colIdx++){
                //Lit la valeur de la touche (i,j)
                val = gpio_get_value(gpiosLire[colIdx]);
                //if(val !=0) 
                    //printk(KERN_INFO "%d %d %d", patternIdx, colIdx, val);

                if (dernierEtat[patternIdx][colIdx]!=val){
                    //Si valeur a changé on enregistre l'etat
                    dernierEtat[patternIdx][colIdx]=val;
                    if (val == 1) {
                    //On écrit la valeur du clavier dans le buffer
                    data[posCouranteEcriture]=valeursClavier[patternIdx][colIdx];
                    //printk(KERN_INFO "pos Ecriture avant: %d\n",posCouranteEcriture);
                    printk(KERN_INFO "ecriture valeur %c\n",data[posCouranteEcriture]);
                    if (posCouranteEcriture<TAILLE_BUFFER){
                        posCouranteEcriture+=1;
                    }
                    else
                        posCouranteEcriture=0;
                    //printk(KERN_INFO "pos Ecriture apres: %d\n",posCouranteEcriture);
                    }
                }
            }
        }
    // 6) Remet toutes les lignes à 1 (pour réarmer l'interruption)
    for (ligneIdx = 0; ligneIdx < 4; ligneIdx++) {
        gpio_set_value(gpiosEcrire[ligneIdx], 1);
    }
    // 7) Réactive le traitement des interruptions
    atomic_set(&irqActif, 1);
}

// On déclare le tasklet avec la macro DECLARE_TASKLET
DECLARE_TASKLET(tasklet_polling, func_tasklet_polling, 0);


static irq_handler_t  setr_irq_handler(unsigned int irq, void *dev_id, struct pt_regs *regs){
    // Ceci est la fonction recevant l'interruption. Son seul rôle consiste à
    // céduler un tasklet qui fera le travail de balayage.
    // Attention toutefois : ce balayage ne doit pas faire en sorte que de _nouvelles_
    // interruptions soient traitées et lancent encore le tasklet, sinon vous vous
    // retrouverez dans une boucle sans fin où le tasklet crée des interruptions,
    // qui lancent le tasklet, qui crée des interruptions, etc.
    // Voyez les commentaires du tasklet pour une piste potentielle de synchronisation.
    // Le seul travail de cette IRQ est de céduler un tasklet qui fera le travail
    // TODO

    // On désactive l'interruption pour éviter que de nouvelles interruptions ne soient traitées pendant
    // le traitement du tasklet.
    disable_irq_nosync(irq);

    // On cède la tâche au tasklet
    tasklet_schedule(&tasklet);

    // On réactive l'interruption une fois que le traitement est terminé
    enable_irq(irq);

    // On retourne en indiquant qu'on a géré l'interruption
    return (irq_handler_t) IRQ_HANDLED;
}


static int __init setrclavier_init(void){
    int i;
    printk(KERN_INFO "SETR_CLAVIER : Initialisation du driver commencee\n");

    majorNumber = register_chrdev(0, DEV_NAME, &fops);
    if (majorNumber<0){
      printk(KERN_ALERT "SETR_CLAVIER : Erreur lors de l'appel a register_chrdev!\n");
      return majorNumber;
    }

    // Création de la classe de périphérique
    setrClasse = class_create(THIS_MODULE, CLS_NAME);
    if (IS_ERR(setrClasse)){
      unregister_chrdev(majorNumber, DEV_NAME);
      printk(KERN_ALERT "SETR_CLAVIER : Erreur lors de la creation de la classe de peripherique\n");
      return PTR_ERR(setrClasse);
    }
    printk(KERN_INFO "EBBChar: device class registered correctly\n");

    // Création du pilote de périphérique associé
    setrDevice = device_create(setrClasse, NULL, MKDEV(majorNumber, 0), NULL, DEV_NAME);
    if (IS_ERR(setrDevice)){
      class_destroy(setrClasse);
      unregister_chrdev(majorNumber, DEV_NAME);
      printk(KERN_ALERT "SETR_CLAVIER : Erreur lors de la creation du pilote de peripherique\n");
      return PTR_ERR(setrDevice);
    }


    // TODO
    // Initialisez les GPIO. Chaque GPIO utilisé doit être enregistré (fonction gpio_request)
    // et se voir donner une direction (gpio_direction_input / gpio_direction_output).
    // Ces opérations peuvent également être combinées si vous trouvez la bonne fonction pour le faire.
    //
    // Assurez-vous que les entrées soient robustes aux rebondissements (bouncing).
    // Vous devez mettre en place un "debouncing" en utilisant le paramètre dureeDebounce défini plus haut.
    //
    // Finalement, vous devez enregistrer une IRQ pour chaque GPIO en entrée. Utilisez
    // pour ce faire gpio_to_irq, ce qui vous donnera le numéro d'interruption lié à un
    // GPIO en particulier, puis appelez request_irq comme présenté plus bas pour
    // enregistrer la fonction de traitement de l'interruption.
    // Attention, cette fonction devra être appelée 4 fois (une fois pour chaque GPIO)!
    //
    // Vous devez également initialiser le mutex de synchronisation.

    // Initialisation des GPIOs
    for (i = 0; i < sizeof(gpiosEcrire); i++) {
        // On enregistre chaque GPIO utilisé
        if (gpio_request(gpiosEcrire[i], gpiosEcrireNoms[i]) < 0) {
            printk(KERN_ALERT "Erreur lors de la demande de la GPIO %d\n", gpiosEcrire[i]);
            return -1;
        }

        // On configure la direction de chaque GPIO (entrée)
        if (gpio_direction_output(gpiosEcrire[i], 0) < 0) {
            printk(KERN_ALERT "Erreur lors de la configuration de la direction de la GPIO %d\n", gpiosEcrire[i]);
            gpio_free(gpiosEcrire[i]);
            return -1;
        }
        /*
        // On configure le debounce pour chaque GPIO
        if (gpio_set_debounce(gpiosEcrire[i], dureeDebounce) < 0) {
            printk(KERN_ALERT "Erreur lors de la configuration du debounce de la GPIO %d\n", gpiosEcrire[i]);
            gpio_free(gpiosEcrire[i]);
            return -1;
        }*/
    }

    for (i = 0; i < sizeof(gpiosLire); i++) {
        // On enregistre chaque GPIO utilisé
        if (gpio_request(gpiosLire[i], gpiosLireNoms[i]) < 0) {
            printk(KERN_ALERT "Erreur lors de la demande de la GPIO %d\n", gpiosLire[i]);
            return -1;
        }

        // On configure la direction de chaque GPIO (entrée)
        if (gpio_direction_input(gpiosLire[i]) < 0) {
            printk(KERN_ALERT "Erreur lors de la configuration de la direction de la GPIO %d\n", gpiosLire[i]);
            gpio_free(gpiosLire[i]);
            return -1;
        }

        // On enregistre chaque IRQ associée à chaque GPIO
        irqId[i] = gpio_to_irq(gpiosLire[i]);
        if (request_irq(irqId[i], (irq_handler_t) setr_irq_handler, IRQF_TRIGGER_RISING, "setr_irq_handler", NULL) < 0) {
            printk(KERN_ALERT "Erreur lors de l'enregistrement de l'IRQ pour la GPIO %d\n", gpiosLire[i]);
            gpio_free(gpiosLire[i]);
            return -1;
        }
    }
/*

    //Request des GPIO écriture + direction
    for ( i = 0 ; i < 4 ; i++){
        if (gpio_request(gpiosEcrire[i],gpiosEcrireNoms[i])<0) {
            printk("output GPIO request failed\n");
        }
        else if (gpio_direction_output(gpiosEcrire[i],0)<0) {
            printk("output GPIO direction failed\n");
        }
        else {
            printk("output GPIO request + direction success\n");
            if (gpio_set_debounce(gpiosEcrire[i],dureeDebounce)<0){
                printk("set gpio debounce time failed\n");
            }
        }   
    }
    //Request des GPIO lecture + direction
    for(i=0;i<3;i++){
        if (gpio_request(gpiosLire[i],gpiosLireNoms[i])<0){
            printk("output GPIO request failed\n");
        }
        else if (gpio_direction_input(gpiosLire[i])<0){
            printk("output GPIO direction failed\n");
        }
        
        else {
            printk("output GPIO request + direction success\n");
        }
        irqId[i] = gpio_to_irq(gpiosLire[i]);
        ok = request_irq(irqId[i],                 // Le numéro de l'interruption, obtenue avec gpio_to_irq
            (irq_handler_t) setr_irq_handler,  // Pointeur vers la routine de traitement de l'interruption
            IRQF_TRIGGER_RISING,               // On veut une interruption sur le front montant (lorsque le bouton est pressé)
            "setr_irq_handler",                // Le nom de notre interruption
            NULL);                             // Paramètre supplémentaire inutile pour vous
        if(ok != 0) {
            printk(KERN_ALERT "Erreur (%d) lors de l'enregistrement IRQ #{%d}!\n", ok, irqId[i]);
        }
    }*/

    // Initialisation du mutex
    mutex_init(&sync);
/*
    for (i = 0; i < 3; i++)
    {
        int irqno = gpio_to_irq(gpiosLire[i]);
        ok = request_irq(irqno,                 // Le numéro de l'interruption, obtenue avec gpio_to_irq
            (irq_handler_t) setr_irq_handler,  // Pointeur vers la routine de traitement de l'interruption
            IRQF_TRIGGER_RISING,               // On veut une interruption sur le front montant (lorsque le bouton est pressé)
            "setr_irq_handler",                // Le nom de notre interruption
            NULL);                             // Paramètre supplémentaire inutile pour vous
        if(ok != 0) {
            printk(KERN_ALERT "Erreur (%d) lors de l'enregistrement IRQ #{%d}!\n", ok, irqno);
        }
    }*/


    printk(KERN_INFO "SETR_CLAVIER : Fin de l'Initialisation!\n"); // Made it! device was initialized

    return 0;
}


static void __exit setrclavier_exit(void){
    int i;
    // TODO
    // Écrivez le code permettant de relâcher (libérer) les GPIO
    // Vous aurez pour cela besoin de la fonction gpio_free
    // Vous devrez également relâcher les interruptions qui ont été
    // précédemment enregistrées. Utilisez free_irq(irqno, NULL)

    // Libération des GPIO
    for (i = 0; i < 4; i++) {
      gpio_set_value(gpiosEcrire[i], 0);
      gpio_free(gpiosEcrire[i]);
    }
    for (i = 0; i < 3; i++) {
      gpio_free(gpiosLire[i]);
    }

    // On retire correctement les différentes composantes du pilote
    device_destroy(setrClasse, MKDEV(majorNumber, 0));
    class_unregister(setrClasse);
    class_destroy(setrClasse);
    unregister_chrdev(majorNumber, DEV_NAME);
    printk(KERN_INFO "SETR_CLAVIER : Terminaison du driver\n");
}




static int dev_open(struct inode *inodep, struct file *filep){
    printk(KERN_INFO "SETR_CLAVIER : Ouverture!\n");
    // Rien à faire ici, si ce n'est retourner une valeur de succès
    return 0;
}
static int dev_release(struct inode *inodep, struct file *filep){
   printk(KERN_INFO "SETR_CLAVIER : Fermeture!\n");
   // Rien à faire ici, si ce n'est retourner une valeur de succès
   return 0;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset){
/*
    // TODO
    // Implémentez cette fonction de lecture
    //
    // Notez que si le reste de votre code est cohérent, elle peut être _exactement_
    // la même que pour le driver par polling. Le reste des explications est simplement
    // un copier coller des explications de l'autre driver.

    // Celle-ci doit copier N caractères dans le buffer fourni en paramètre, N étant le minimum
    // entre le nombre d'octets disponibles dans le buffer et le nombre d'octets demandés (paramètre len).
    // Cette fonction DOIT se synchroniser au reste du module avec le mutex.
    // N'oubliez pas d'utiliser copy_to_user et NON memcpy pour copier les données dans le buffer
    // de l'utilisateur!
    // Finalement, rappelez-vous que nous utilisons un buffer circulaire, c'est à dire que les nouvelles
    // écritures se font sur des adresses croissantes, jusqu'à ce qu'on arrive à la fin du buffer et qu'on
    // revienne alors à 0. Il est donc tout à fait possible que posCouranteEcriture soit INFÉRIEUR à
    // posCouranteLecture, et vous devez gérer ce cas sans perdre de caractères et en respectant les
    // autres conditions (par exemple, ne jamais copier plus que len caractères).
    ssize_t bytesRead = 0;

    // On verrouille le mutex
    //mutex_lock_interruptible(&sync);
    mutex_lock(&sync);


    // Tant qu'il y a des octets à lire dans le buffer
    while (posCouranteLecture != posCouranteEcriture) {
        // On calcule le nombre d'octets à lire pour cette itération
        size_t bytesToRead = min(len - bytesRead, (size_t) (posCouranteEcriture - posCouranteLecture));

        // On lit les octets depuis le buffer circulaire vers le buffer utilisateur
        if (copy_to_user(buffer + bytesRead, data + posCouranteLecture, bytesToRead)) {
            printk(KERN_ERR "Erreur lors de la copie des données depuis le buffer circulaire vers le buffer utilisateur.\n");
            mutex_unlock(&sync);
            return -EFAULT;
        }

        // On met à jour le nombre d'octets lus
        bytesRead += bytesToRead;

        // On met à jour la position de lecture dans le buffer circulaire
        posCouranteLecture = (posCouranteLecture + bytesToRead) % TAILLE_BUFFER;
    }

    // On déverrouille le mutex
    mutex_unlock(&sync);

    // On retourne le nombre d'octets lus
    return bytesRead;*/

    // TODO
    // Implémentez cette fonction de lecture
    // Celle-ci doit copier N caractères dans le buffer fourni en paramètre, N étant le minimum
    // entre le nombre d'octets disponibles dans le buffer et le nombre d'octets demandés (paramètre len).
    // Cette fonction DOIT se synchroniser au reste du module avec le mutex.
    // N'oubliez pas d'utiliser copy_to_user et NON memcpy pour copier les données dans le buffer
    // de l'utilisateur!
    // Finalement, rappelez-vous que nous utilisons un buffer circulaire, c'est à dire que les nouvelles
    // écritures se font sur des adresses croissantes, jusqu'à ce qu'on arrive à la fin du buffer et qu'on
    // revienne alors à 0. Il est donc tout à fait possible que posCouranteEcriture soit INFÉRIEUR à
    // posCouranteLecture, et vous devez gérer ce cas sans perdre de caractères et en respectant les
    // autres conditions (par exemple, ne jamais copier plus que len caractères).
    int c;
    int nbr_a_copier=0;

    //est-ce qu'il faut vérifier poscouranteEcriture >= posCouranteLecture?

    //TODO
    //Mettre decalage a data (difference entre posCouranteEcriture et posCouranteLecture)
    //Attention a pas copier longueur len. len c'est le max qu'on peut copier mais nous on prend que ce qui est dispo 
    // de plus depuis la derniere fois (difference entre posCouranteEcriture et posCouranteLecture)
    //du coup on copie le min entre len et ce qui est dispo



    mutex_lock(&sync);
    //printk(KERN_INFO "Contenu buffer : [0]=%hhu [1]=%hhu [2]=%hhu, [3]=%hhu, [4]=%hhu, [5]=%hhu, [6]=%hhu, [7]=%hhu \n", data[0], data[1], data[2], data[3], data[4],data[5], data[6], data[7]);
    //printk(KERN_INFO "nbr_a_copier=%d, posEcriture=%u, posLecture=%u, len=%u\n", nbr_a_copier, posCouranteEcriture, posCouranteLecture, len);
    if (posCouranteEcriture  > posCouranteLecture){
      //Cas 1 : on a pas rebouclé sur le début du buffer
        nbr_a_copier = posCouranteEcriture-posCouranteLecture;
        //On copie les données si il y en a pas trop
        if ((nbr_a_copier<len) && (nbr_a_copier<(TAILLE_BUFFER-posCouranteEcriture+posCouranteLecture))){
            //if(nbr_a_copier > 0)
            //printk(KERN_INFO "nbr_a_copier=%u, data[%u]=%hhu", nbr_a_copier, posCouranteLecture, data[posCouranteLecture]);
            
            c = copy_to_user(buffer, data+posCouranteLecture, nbr_a_copier);
            //Verif de la réussite de la copie
            if (c > 0) {
                printk(KERN_INFO "Not all bytes copied, left = %d\n",c);
                posCouranteLecture+=nbr_a_copier-c;
            }
            else{
                //printk(KERN_INFO "copie reussie\n");
                posCouranteLecture+=nbr_a_copier;
            }
        }
        else
            printk(KERN_INFO "quantité à copier dépasse len\n");
        }

        else if (posCouranteEcriture<posCouranteLecture){
        //Cas 2 : on a commencé à réécrire au début du buffer
        nbr_a_copier = TAILLE_BUFFER-posCouranteLecture+posCouranteEcriture;
        //On copie les données si il y en a pas trop
        if ((nbr_a_copier<len) && (nbr_a_copier<(posCouranteLecture-posCouranteEcriture))){
            c = copy_to_user(buffer, data+posCouranteLecture, nbr_a_copier);
            //Verif de la réussite de la copie
            if (c > 0) {
            printk(KERN_INFO "Not all bytes copied, left = %d\n",c);
            posCouranteLecture+=nbr_a_copier-c;
            }
            else{
            //printk(KERN_INFO "copie reussie\n");
            posCouranteLecture+=nbr_a_copier;
            }
        }
        else
            printk(KERN_INFO "quantité à copier dépasse len\n");
        }
        mutex_unlock(&sync);
  return nbr_a_copier;
}


// On enregistre les fonctions d'initialisation et de destruction
module_init(setrclavier_init);
module_exit(setrclavier_exit);

// Description du module
MODULE_LICENSE("GPL");            // Licence : laissez "GPL"
MODULE_AUTHOR("Vous!");           // Vos noms
MODULE_DESCRIPTION("Lecteur de clavier externe, avec interruptions");  // Description du module
MODULE_VERSION("0.3");            // Numéro de version
