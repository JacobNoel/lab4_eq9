/******************************************************************************
* H2023
* LABORATOIRE 4, Systèmes embarqués et temps réel
* Ébauche de code pour le pilote utilisant le polling
* Marc-André Gardner, mars 2019
*
* Ce fichier contient la structure du pilote qu'il vous faut implémenter. Ce
* pilote fonctionne en mode "polling", c'est-à-dire qu'il vérifie en permanance
* si un événement (pression d'une touche) s'est produit, grâce à un thread noyau.
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
#include <linux/kthread.h>          // Utilisation des threads noyau
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

static struct mutex sync;                          // Mutex servant à synchroniser les accès au buffer
static struct task_struct *task;            // Réfère au thread noyau

// 4 GPIO doivent être assignés pour l'écriture, et 4 en lecture (voir énoncé)
// Nous vous proposons les choix suivants, mais ce n'est pas obligatoire
static int  gpiosEcrire[] = {5, 6, 13, 19};             // Correspond aux pins 29, 31, 33 et 35
static int  gpiosLire[] = {12, 16, 20};             // Correspond aux pins 32, 36, 38, et 40
// Les noms des différents GPIO
static char* gpiosEcrireNoms[] = {"OUT1", "OUT2", "OUT3", "OUT4"};
static char* gpiosLireNoms[] = {"IN1", "IN2", "IN3"};

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



// Vous devez déclarer cette variable comme paramètre
static unsigned int pausePollingMs = 20;
module_param(pausePollingMs, uint, S_IRUGO);
MODULE_PARM_DESC(pausePollingMs, " Duree de la pause apres chaque polling (en ms, 20ms par defaut)");

// Durée (en ms) du "debounce" des touches
static int dureeDebounce = 50;



static int pollClavier(void *arg){
    // Cette fonction contient la boucle principale du thread détectant une pression sur une touche
    int patternIdx, ligneIdx, colIdx, val;
    printk(KERN_INFO "SETR_CLAVIER : Poll clavier declenche! \n");
    while(!kthread_should_stop()){           // Permet de s'arrêter en douceur lorsque kthread_stop() sera appelé
      set_current_state(TASK_RUNNING);      // On indique qu'on est en train de faire quelque chose

      // TODO
      // Écrivez le code permettant
      // 1) De passer au travers de tous les patrons de balayage
      // 2) Pour chaque patron, vérifier la valeur des lignes d'entrée
      // 3) Selon ces valeurs et le contenu de dernierEtat, déterminer si une nouvelle touche a été pressée
      // 4) Mettre à jour le buffer et dernierEtat en vous assurant d'éviter les race conditions avec le reste du module

      //Boucle sur chaque ligne de pattern qui correspond à un patron de balayage
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
      set_current_state(TASK_INTERRUPTIBLE); // On indique qu'on peut etre interrompu
      msleep(pausePollingMs);                // On se met en pause un certain temps
    }
    printk(KERN_INFO "SETR_CLAVIER : Poll clavier stop! \n");
    return 0;
}


static int __init setrclavier_init(void){
    int i;
    printk(KERN_INFO "SETR_CLAVIER : Initialisation du driver commencee\n");

    // On enregistre notre pilote
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
    // Finalement, assurez-vous que les entrées soient robustes aux rebondissements (bouncing).
    // Vous devez mettre en place un "debouncing" en utilisant le paramètre dureeDebounce défini plus haut.
    //
    // Vous devez également initialiser le mutex de synchronisation.

    //Est-ce qu'il faut check que le gpio est valide???
    

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
        };
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
    }

    //Initialisation du mutex --- c'est tout???
    mutex_init(&sync);


    // Le mutex devrait avoir été initialisé avant d'appeler la ligne suivante!
    task = kthread_run(pollClavier, NULL, "Thread_polling_clavier");

    printk(KERN_INFO "SETR_CLAVIER : Fin de l'Initialisation!\n"); // Made it! device was initialized

    return 0;
}


static void __exit setrclavier_exit(void){
    int i;

    // On arrête le thread de lecture
    kthread_stop(task);

    // TODO
    // Écrivez le code permettant de relâcher (libérer) les GPIO
    // Vous aurez pour cela besoin de la fonction gpio_free

    for (i=0;i<4;i++) {
      gpio_set_value(gpiosEcrire[i],0);
      gpio_free(gpiosEcrire[i]);
    }
    for (i=0;i<3;i++) {
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
    int i=0;
    int nbr_a_copier=0;

    //est-ce qu'il faut vérifier poscouranteEcriture >= posCouranteLecture?

    //TODO
    //Mettre decalage a data (difference entre posCouranteEcriture et posCouranteLecture)
    //Attention a pas copier longueur len. len c'est le max qu'on peut copier mais nous on prend que ce qui est dispo 
    // de plus depuis la derniere fois (difference entre posCouranteEcriture et posCouranteLecture)
    //du coup on copie le min entre len et ce qui est dispo



    mutex_lock(&sync);
    //printk(KERN_INFO "taille buffer %d",TAILLE_BUFFER);
    //printk(KERN_INFO "position ecriture %d",posCouranteEcriture);
    //printk(KERN_INFO "position lecture:%d\n",posCouranteLecture);

    if (posCouranteEcriture  > posCouranteLecture){
      //Cas 1 : on a pas rebouclé sur le début du buffer
      nbr_a_copier = posCouranteEcriture-posCouranteLecture;
      //On copie les données si il y en a pas trop
      if ((nbr_a_copier<len) && (nbr_a_copier<(TAILLE_BUFFER-posCouranteEcriture+posCouranteLecture))){
        c = copy_to_user(buffer, data+posCouranteEcriture, nbr_a_copier);
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
        c = copy_to_user(buffer, data+posCouranteEcriture, nbr_a_copier);
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



  return 0;
}

// On enregistre les fonctions d'initialisation et de destruction
module_init(setrclavier_init);
module_exit(setrclavier_exit);

// Description du module
MODULE_LICENSE("GPL");            // Licence : laissez "GPL"
MODULE_AUTHOR("Vous!");           // Vos noms
MODULE_DESCRIPTION("Lecteur de clavier externe");  // Description du module
MODULE_VERSION("0.3");            // Numéri de version
